#pragma once

#include "chronoring/event_frame.hpp"
#include "chronoring/simd_trigger.hpp"
#include "chronoring/status.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace chronoring {

enum class OffloadBackendKind : std::uint8_t {
    CpuSimd    = 0,
    IoUring    = 1,
    Dpdk       = 2,
    CudaHipGpu = 3,
    FpgaOpenCl = 4
};

struct OffloadCapabilities {
    OffloadBackendKind kind{OffloadBackendKind::CpuSimd};
    std::string device_name{};
    bool available{false};
    std::size_t preferred_batch_events{kColumnarPageEvents};
};

struct OffloadResult {
    RingStatus status{RingStatus::Ok};
    std::size_t passed_events{0};
    std::uint64_t device_time_ns{0};
    OffloadBackendKind executed_on{OffloadBackendKind::CpuSimd};
};

// Abstract hardware offload trigger stage operating directly on aligned SoA ColumnarPages.
class TriggerOffloadDevice {
public:
    virtual ~TriggerOffloadDevice() = default;

    [[nodiscard]] virtual OffloadCapabilities capabilities() const = 0;

    [[nodiscard]] virtual OffloadResult submit_soa_page(
        const ColumnarPage& page,
        const TriggerConfig& cfg,
        std::uint64_t* out_bitmask_words) noexcept = 0;
};

class CpuSimdOffloadDevice final : public TriggerOffloadDevice {
public:
    explicit CpuSimdOffloadDevice(SimdBackend backend = SimdBackend::Auto) noexcept
        : backend_(backend) {}

    [[nodiscard]] OffloadCapabilities capabilities() const override;

    [[nodiscard]] OffloadResult submit_soa_page(
        const ColumnarPage& page,
        const TriggerConfig& cfg,
        std::uint64_t* out_bitmask_words) noexcept override;

private:
    SimdBackend backend_{SimdBackend::Auto};
};

class IoUringOffloadDevice final : public TriggerOffloadDevice {
public:
    IoUringOffloadDevice() noexcept;

    [[nodiscard]] OffloadCapabilities capabilities() const override;

    [[nodiscard]] OffloadResult submit_soa_page(
        const ColumnarPage& page,
        const TriggerConfig& cfg,
        std::uint64_t* out_bitmask_words) noexcept override;

private:
    bool kernel_supported_{false};
};

// Zero-copy SoA offload adapter/emulator for DPDK, CUDA/HIP GPU, and OpenCL FPGA stages.
class SimulatedAcceleratorOffloadDevice final : public TriggerOffloadDevice {
public:
    SimulatedAcceleratorOffloadDevice(OffloadBackendKind kind,
                                      std::string device_name,
                                      bool available = true) noexcept;

    [[nodiscard]] OffloadCapabilities capabilities() const override;

    [[nodiscard]] OffloadResult submit_soa_page(
        const ColumnarPage& page,
        const TriggerConfig& cfg,
        std::uint64_t* out_bitmask_words) noexcept override;

private:
    OffloadBackendKind kind_;
    std::string device_name_;
    bool available_;
};

// Routes SoA pages to the preferred registered hardware offload device,
// falling back transparently to CpuSimd if the accelerator is offline or unavailable.
class OffloadStageRouter {
public:
    OffloadStageRouter() noexcept;

    void register_device(TriggerOffloadDevice* device) noexcept;
    void set_preferred(OffloadBackendKind kind) noexcept { preferred_ = kind; }
    [[nodiscard]] OffloadBackendKind preferred() const noexcept { return preferred_; }

    [[nodiscard]] bool is_available(OffloadBackendKind kind) const noexcept;
    [[nodiscard]] OffloadResult execute_page(
        const ColumnarPage& page,
        const TriggerConfig& cfg,
        std::uint64_t* out_bitmask_words) const noexcept;

private:
    mutable CpuSimdOffloadDevice fallback_cpu_{};
    std::array<TriggerOffloadDevice*, 5> devices_{};
    OffloadBackendKind preferred_{OffloadBackendKind::CpuSimd};
};

} // namespace chronoring
