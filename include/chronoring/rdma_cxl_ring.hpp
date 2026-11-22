#pragma once

#include "chronoring/event_frame.hpp"
#include "chronoring/pcie_cxl_ring.hpp"
#include "chronoring/shm_ring.hpp"
#include "chronoring/status.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>

namespace chronoring {

enum class FabricTransportKind : std::uint8_t {
    SharedMemory   = 0,
    CxlMemCoherent = 1,
    RdmaRoceV2     = 2,
    RdmaInfiniBand = 3
};

struct RdmaRegionDescriptor {
    std::uint64_t remote_addr{0};
    std::uint32_t rkey{0};
    std::uint32_t lkey{0};
    std::size_t length_bytes{0};
    std::size_t capacity{0};
};

// Abstract one-sided RDMA QP / memory-window transport interface.
class RdmaTransport {
public:
    virtual ~RdmaTransport() = default;

    [[nodiscard]] virtual RingStatus rdma_write(
        const RdmaRegionDescriptor& rdesc,
        std::size_t remote_offset,
        const void* local_src,
        std::size_t len) noexcept = 0;

    [[nodiscard]] virtual RingStatus rdma_read(
        const RdmaRegionDescriptor& rdesc,
        std::size_t remote_offset,
        void* local_dst,
        std::size_t len) const noexcept = 0;

    [[nodiscard]] virtual RingStatus rdma_write_imm_doorbell(
        const RdmaRegionDescriptor& rdesc,
        std::uint64_t new_head) noexcept = 0;
};

// Memory-backed loopback RDMA NIC / RoCEv2 simulator enforcing rkey & MR bounds validation
// and performing direct zero-CPU-involvement updates to the target PcieCxlRingHeader.
class LoopbackRdmaTransport final : public RdmaTransport {
public:
    [[nodiscard]] RdmaRegionDescriptor register_memory(void* base_addr,
                                                       std::size_t length_bytes,
                                                       std::size_t capacity,
                                                       std::uint32_t rkey);

    void unregister_memory(std::uint32_t rkey);

    [[nodiscard]] RingStatus rdma_write(
        const RdmaRegionDescriptor& rdesc,
        std::size_t remote_offset,
        const void* local_src,
        std::size_t len) noexcept override;

    [[nodiscard]] RingStatus rdma_read(
        const RdmaRegionDescriptor& rdesc,
        std::size_t remote_offset,
        void* local_dst,
        std::size_t len) const noexcept override;

    [[nodiscard]] RingStatus rdma_write_imm_doorbell(
        const RdmaRegionDescriptor& rdesc,
        std::uint64_t new_head) noexcept override;

private:
    struct RegisteredMr {
        std::uint8_t* base{nullptr};
        std::size_t length{0};
        std::uint32_t rkey{0};
    };

    std::unordered_map<std::uint64_t, RegisteredMr> mrs_{};
};

// Unified zero-copy distributed ring endpoint spanning Shared Memory (SHM),
// CXL.mem Type-3 coherent memory, and one-sided RDMA (RoCEv2 / InfiniBand).
class FabricRingEndpoint {
public:
    FabricRingEndpoint() = default;

    [[nodiscard]] RingStatus open_shm(const std::string& name,
                                      ShmMode mode,
                                      std::size_t capacity = 1024,
                                      bool unlink_on_destroy = false);

    [[nodiscard]] RingStatus open_cxl_pinned(std::size_t capacity,
                                             std::uint64_t bus_addr = 0);

    [[nodiscard]] RingStatus open_rdma_producer(
        RdmaTransport* transport,
        RdmaRegionDescriptor remote_desc,
        FabricTransportKind kind = FabricTransportKind::RdmaRoceV2) noexcept;

    [[nodiscard]] RingStatus attach_rdma_consumer(
        void* local_pinned_base,
        std::size_t byte_length,
        std::size_t capacity,
        FabricTransportKind kind = FabricTransportKind::RdmaRoceV2) noexcept;

    [[nodiscard]] bool is_open() const noexcept { return is_open_; }
    [[nodiscard]] FabricTransportKind transport_kind() const noexcept { return kind_; }
    [[nodiscard]] void* cxl_or_rdma_base() noexcept;

    [[nodiscard]] std::size_t try_push_batch(std::span<const EventFrame> frames) noexcept;
    [[nodiscard]] std::size_t try_pop_batch(std::span<EventFrame> out) noexcept;
    [[nodiscard]] std::size_t size_approx() const noexcept;
    [[nodiscard]] RingStatus last_status() const noexcept { return last_status_; }

private:
    bool is_open_{false};
    FabricTransportKind kind_{FabricTransportKind::SharedMemory};
    RingStatus last_status_{RingStatus::Ok};

    std::unique_ptr<ShmRingBuffer> shm_ring_{};
    PcieCxlRing cxl_ring_{};
    std::unique_ptr<MockFpgaDmaProducer> cxl_writer_{};

    RdmaTransport* rdma_transport_{nullptr};
    RdmaRegionDescriptor rdma_desc_{};
    std::uint64_t rdma_head_{0};
    std::uint64_t rdma_cached_tail_{0};
};

} // namespace chronoring
