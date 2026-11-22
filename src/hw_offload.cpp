#include "chronoring/hw_offload.hpp"

#include <chrono>
#include <cstring>
#include <utility>

#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#if defined(__NR_io_uring_setup) && __has_include(<linux/io_uring.h>)
#include <linux/io_uring.h>
#define CHRONORING_HAS_IO_URING 1
#endif
#endif

namespace chronoring {

namespace {

std::uint64_t now_ns() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

} // namespace

// ============================================================================
// CpuSimdOffloadDevice
// ============================================================================

OffloadCapabilities CpuSimdOffloadDevice::capabilities() const {
    return OffloadCapabilities{
        OffloadBackendKind::CpuSimd,
        std::string("CPU-") + backend_name(active_backend(backend_)),
        true,
        kColumnarPageEvents};
}

OffloadResult CpuSimdOffloadDevice::submit_soa_page(
    const ColumnarPage& page,
    const TriggerConfig& cfg,
    std::uint64_t* out_bitmask_words) noexcept {
    const std::uint64_t t0 = now_ns();
    const std::size_t passed =
        evaluate_columnar_page(page, cfg, out_bitmask_words, backend_);
    const std::uint64_t t1 = now_ns();
    return OffloadResult{
        RingStatus::Ok,
        passed,
        (t1 >= t0) ? (t1 - t0) : 0ULL,
        OffloadBackendKind::CpuSimd};
}

// ============================================================================
// IoUringOffloadDevice
// ============================================================================

IoUringOffloadDevice::IoUringOffloadDevice() noexcept {
#ifdef CHRONORING_HAS_IO_URING
    struct io_uring_params params{};
    std::memset(&params, 0, sizeof(params));
    const long fd = ::syscall(__NR_io_uring_setup, 8u, &params);
    if (fd >= 0) {
        ::close(static_cast<int>(fd));
        kernel_supported_ = true;
    }
#endif
}

OffloadCapabilities IoUringOffloadDevice::capabilities() const {
    return OffloadCapabilities{
        OffloadBackendKind::IoUring,
        "Linux-io_uring",
        kernel_supported_,
        kColumnarPageEvents};
}

OffloadResult IoUringOffloadDevice::submit_soa_page(
    const ColumnarPage& page,
    const TriggerConfig& cfg,
    std::uint64_t* out_bitmask_words) noexcept {
    if (!kernel_supported_) {
        return OffloadResult{RingStatus::IoError, 0, 0, OffloadBackendKind::IoUring};
    }
    const std::uint64_t t0 = now_ns();
    const std::size_t passed =
        evaluate_columnar_page(page, cfg, out_bitmask_words, SimdBackend::Auto);
    const std::uint64_t t1 = now_ns();
    return OffloadResult{
        RingStatus::Ok,
        passed,
        (t1 >= t0) ? (t1 - t0) : 0ULL,
        OffloadBackendKind::IoUring};
}

// ============================================================================
// SimulatedAcceleratorOffloadDevice
// ============================================================================

SimulatedAcceleratorOffloadDevice::SimulatedAcceleratorOffloadDevice(
    OffloadBackendKind kind,
    std::string device_name,
    bool available) noexcept
    : kind_(kind), device_name_(std::move(device_name)), available_(available) {}

OffloadCapabilities SimulatedAcceleratorOffloadDevice::capabilities() const {
    return OffloadCapabilities{
        kind_,
        device_name_,
        available_,
        kColumnarPageEvents};
}

OffloadResult SimulatedAcceleratorOffloadDevice::submit_soa_page(
    const ColumnarPage& page,
    const TriggerConfig& cfg,
    std::uint64_t* out_bitmask_words) noexcept {
    if (!available_) {
        return OffloadResult{RingStatus::Closed, 0, 0, kind_};
    }
    const std::uint64_t t0 = now_ns();
    // Execute bit-exact SoA predicate evaluation matching hardware kernel semantics.
    const std::size_t passed =
        evaluate_columnar_page(page, cfg, out_bitmask_words, SimdBackend::Auto);
    const std::uint64_t t1 = now_ns();
    return OffloadResult{
        RingStatus::Ok,
        passed,
        (t1 >= t0) ? (t1 - t0) : 0ULL,
        kind_};
}

// ============================================================================
// OffloadStageRouter
// ============================================================================

OffloadStageRouter::OffloadStageRouter() noexcept {
    devices_[static_cast<std::size_t>(OffloadBackendKind::CpuSimd)] = &fallback_cpu_;
}

void OffloadStageRouter::register_device(TriggerOffloadDevice* device) noexcept {
    if (device == nullptr) {
        return;
    }
    const std::size_t idx = static_cast<std::size_t>(device->capabilities().kind);
    if (idx < devices_.size()) {
        devices_[idx] = device;
    }
}

bool OffloadStageRouter::is_available(OffloadBackendKind kind) const noexcept {
    const std::size_t idx = static_cast<std::size_t>(kind);
    return idx < devices_.size() && devices_[idx] != nullptr &&
           devices_[idx]->capabilities().available;
}

OffloadResult OffloadStageRouter::execute_page(
    const ColumnarPage& page,
    const TriggerConfig& cfg,
    std::uint64_t* out_bitmask_words) const noexcept {
    const std::size_t idx = static_cast<std::size_t>(preferred_);
    if (idx < devices_.size() && devices_[idx] != nullptr &&
        devices_[idx]->capabilities().available) {
        const OffloadResult res =
            devices_[idx]->submit_soa_page(page, cfg, out_bitmask_words);
        if (res.status == RingStatus::Ok) {
            return res;
        }
    }
    return fallback_cpu_.submit_soa_page(page, cfg, out_bitmask_words);
}

} // namespace chronoring
