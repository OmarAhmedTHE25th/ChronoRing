#pragma once

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4324) // structure was padded due to alignment specifier
#endif

#include "chronoring/event_frame.hpp"
#include "chronoring/status.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace chronoring {

inline constexpr std::uint32_t kPcieCxlMagic = 0x43584C52u; // "CXLR"
inline constexpr std::uint16_t kPcieCxlVersion = 1u;

enum class DmaTransportKind : std::uint16_t {
    PcieBarMmio = 0, // PCIe BAR / VFIO peer-to-peer memory-mapped region
    CxlMemDax   = 1, // CXL.mem Type-3 coherent memory (/dev/dax*)
    HostPinned  = 2  // Host pinned / hugepage DMA buffer
};

// Hardware-accessible 192-byte (3 cache line) DMA control header placed at offset 0
// of a PCIe BAR, CXL.mem DAX window, or pinned DMA buffer.
//
// Zero-CPU producer protocol:
//   1. Detector frontend FPGA writes EventFrame(s) directly to slots[head & mask]
//      via PCIe Posted Memory Writes (MWr) or CXL.mem coherent stores.
//   2. FPGA updates `device_head` (release / PCIe write ordering barrier) — zero CPU involvement.
//   3. Host consumer reads `device_head` with acquire semantics, processes EventFrames
//      in-place (zero-copy), and writes `host_tail` back to the doorbell so the FPGA
//      reclaims freed DMA ring slots.
struct alignas(kCacheLineSize) PcieCxlRingHeader {
    std::uint32_t magic{kPcieCxlMagic};
    std::uint16_t version{kPcieCxlVersion};
    std::uint16_t transport_kind{static_cast<std::uint16_t>(DmaTransportKind::PcieBarMmio)};
    std::uint64_t capacity{0};
    std::uint64_t element_size{sizeof(EventFrame)};
    std::uint64_t dma_bus_addr{0};
    std::atomic<std::uint32_t> ready_flag{0};
    std::atomic<std::uint32_t> device_closed_flag{0};
    std::uint8_t pad0[24]{};

    // Cache line 1: Written exclusively by FPGA / P2P DMA producer.
    alignas(kCacheLineSize) std::atomic<std::uint64_t> device_head{0};
    std::atomic<std::uint64_t> device_dropped_frames{0};
    std::uint64_t pad1[6]{};

    // Cache line 2: Written exclusively by host consumer (doorbell back to FPGA).
    alignas(kCacheLineSize) std::atomic<std::uint64_t> host_tail{0};
    std::uint64_t pad2[7]{};
};

static_assert(sizeof(PcieCxlRingHeader) == 192, "PcieCxlRingHeader must occupy 3 cache lines");
static_assert(alignof(PcieCxlRingHeader) == 64, "PcieCxlRingHeader must be 64-byte aligned");
static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "64-bit atomics must be lock-free for PCIe/CXL DMA rings");

// Host-side zero-copy consumer for a direct PCIe / CXL peer-to-peer FPGA ring.
class PcieCxlRing {
public:
    PcieCxlRing() noexcept = default;
    ~PcieCxlRing();

    PcieCxlRing(const PcieCxlRing&) = delete;
    PcieCxlRing& operator=(const PcieCxlRing&) = delete;
    PcieCxlRing(PcieCxlRing&& other) noexcept;
    PcieCxlRing& operator=(PcieCxlRing&& other) noexcept;

    [[nodiscard]] static constexpr std::size_t required_bytes(std::size_t capacity) noexcept {
        return sizeof(PcieCxlRingHeader) + capacity * sizeof(EventFrame);
    }

    // Attach to an already-mapped PCIe BAR / VFIO / CXL.mem / pinned DMA region.
    [[nodiscard]] RingStatus attach_mmio(void* base_addr,
                                         std::size_t byte_length,
                                         std::size_t capacity,
                                         DmaTransportKind kind = DmaTransportKind::PcieBarMmio,
                                         std::uint64_t dma_bus_addr = 0,
                                         bool initialize_header = true) noexcept;

    // Map a Linux PCIe resource file (/sys/bus/pci/.../resourceN) or CXL DAX device (/dev/dax*).
    [[nodiscard]] RingStatus open_device(const std::string& device_path,
                                         std::size_t capacity,
                                         DmaTransportKind kind = DmaTransportKind::CxlMemDax,
                                         bool initialize_header = false);

    // Allocate a host-pinned DMA-capable buffer (uses mmap + best-effort mlock on POSIX).
    [[nodiscard]] RingStatus allocate_pinned(std::size_t capacity,
                                             std::uint64_t dma_bus_addr = 0) noexcept;

    void detach() noexcept;

    [[nodiscard]] bool is_open() const noexcept { return header_ != nullptr; }
    [[nodiscard]] bool is_device_closed() const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] DmaTransportKind transport_kind() const noexcept;
    [[nodiscard]] std::uint64_t dma_bus_addr() const noexcept;
    [[nodiscard]] std::uint64_t device_dropped_frames() const noexcept;
    [[nodiscard]] void* raw_base() noexcept { return mapping_base_; }
    [[nodiscard]] const void* raw_base() const noexcept { return mapping_base_; }
    [[nodiscard]] RingStatus last_status() const noexcept { return last_status_; }
    [[nodiscard]] const std::string& last_error() const noexcept { return last_error_; }

    // Host consumer operations (zero-copy peek/consume and batch pop).
    [[nodiscard]] const EventFrame* try_peek() noexcept;
    void consume_pop() noexcept;
    [[nodiscard]] RingStatus try_pop_status(EventFrame& out) noexcept;
    [[nodiscard]] std::size_t try_pop_batch(EventFrame* out, std::size_t max_count) noexcept;
    [[nodiscard]] std::size_t try_pop_batch(std::span<EventFrame> out) noexcept {
        return try_pop_batch(out.data(), out.size());
    }
    [[nodiscard]] std::size_t size_approx() const noexcept;
    [[nodiscard]] bool empty() const noexcept { return size_approx() == 0; }

private:
    enum class Ownership : std::uint8_t {
        None = 0,
        ExternalMmio,
        DeviceMmap,
        HostPinnedAlloc
    };

    void* mapping_base_{nullptr};
    std::size_t mapped_bytes_{0};
    PcieCxlRingHeader* header_{nullptr};
    EventFrame* slots_{nullptr};
    std::size_t capacity_{0};
    std::size_t mask_{0};

    std::uint64_t cached_device_head_{0};
    std::uint64_t local_host_tail_{0};

    Ownership ownership_{Ownership::None};
    int fd_{-1};
    RingStatus last_status_{RingStatus::Ok};
    std::string last_error_{};
};

// Simulates an FPGA DMA engine writing EventFrames directly into the raw PCIe/CXL
// memory buffer and ringing `device_head` with zero host CPU participation.
class MockFpgaDmaProducer {
public:
    explicit MockFpgaDmaProducer(void* dma_base) noexcept;

    [[nodiscard]] bool is_ready() const noexcept;
    [[nodiscard]] std::size_t dma_write_burst(const EventFrame* frames,
                                              std::size_t count) noexcept;
    [[nodiscard]] std::size_t dma_write_burst(std::span<const EventFrame> frames) noexcept {
        return dma_write_burst(frames.data(), frames.size());
    }
    void signal_device_close() noexcept;

private:
    PcieCxlRingHeader* header_{nullptr};
    EventFrame* slots_{nullptr};
    std::size_t capacity_{0};
    std::size_t mask_{0};
    std::uint64_t fpga_head_{0};
    std::uint64_t cached_host_tail_{0};
};

} // namespace chronoring

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
