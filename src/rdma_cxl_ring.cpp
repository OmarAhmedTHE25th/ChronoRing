#include "chronoring/rdma_cxl_ring.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace chronoring {

// ============================================================================
// LoopbackRdmaTransport
// ============================================================================

RdmaRegionDescriptor LoopbackRdmaTransport::register_memory(void* base_addr,
                                                            std::size_t length_bytes,
                                                            std::size_t capacity,
                                                            std::uint32_t rkey) {
    const auto addr = reinterpret_cast<std::uint64_t>(base_addr);
    mrs_[rkey] = RegisteredMr{static_cast<std::uint8_t*>(base_addr), length_bytes, rkey};
    RdmaRegionDescriptor desc{};
    desc.remote_addr = addr;
    desc.rkey = rkey;
    desc.lkey = rkey;
    desc.length_bytes = length_bytes;
    desc.capacity = capacity;
    return desc;
}

void LoopbackRdmaTransport::unregister_memory(std::uint32_t rkey) {
    mrs_.erase(rkey);
}

RingStatus LoopbackRdmaTransport::rdma_write(const RdmaRegionDescriptor& rdesc,
                                             std::size_t remote_offset,
                                             const void* local_src,
                                             std::size_t len) noexcept {
    const auto it = mrs_.find(rdesc.rkey);
    if (it == mrs_.end() || it->second.base == nullptr) {
        return RingStatus::MappingError;
    }
    if (local_src == nullptr || remote_offset + len > it->second.length) {
        return RingStatus::InvalidArgument;
    }
    if (len > 0) {
        std::memcpy(it->second.base + remote_offset, local_src, len);
    }
    return RingStatus::Ok;
}

RingStatus LoopbackRdmaTransport::rdma_read(const RdmaRegionDescriptor& rdesc,
                                            std::size_t remote_offset,
                                            void* local_dst,
                                            std::size_t len) const noexcept {
    const auto it = mrs_.find(rdesc.rkey);
    if (it == mrs_.end() || it->second.base == nullptr) {
        return RingStatus::MappingError;
    }
    if (local_dst == nullptr || remote_offset + len > it->second.length) {
        return RingStatus::InvalidArgument;
    }
    if (len > 0) {
        std::memcpy(local_dst, it->second.base + remote_offset, len);
    }
    return RingStatus::Ok;
}

RingStatus LoopbackRdmaTransport::rdma_write_imm_doorbell(const RdmaRegionDescriptor& rdesc,
                                                          std::uint64_t new_head) noexcept {
    const auto it = mrs_.find(rdesc.rkey);
    if (it == mrs_.end() || it->second.base == nullptr ||
        it->second.length < sizeof(PcieCxlRingHeader)) {
        return RingStatus::MappingError;
    }
    auto* hdr = reinterpret_cast<PcieCxlRingHeader*>(it->second.base);
    hdr->device_head.store(new_head, std::memory_order_release);
    return RingStatus::Ok;
}

// ============================================================================
// FabricRingEndpoint
// ============================================================================

RingStatus FabricRingEndpoint::open_shm(const std::string& name,
                                        ShmMode mode,
                                        std::size_t capacity,
                                        bool unlink_on_destroy) {
    kind_ = FabricTransportKind::SharedMemory;
    shm_ring_ = std::make_unique<ShmRingBuffer>(name, mode, capacity, unlink_on_destroy);
    if (!shm_ring_->is_open()) {
        last_status_ = shm_ring_->last_status();
        is_open_ = false;
        return last_status_;
    }
    is_open_ = true;
    last_status_ = RingStatus::Ok;
    return RingStatus::Ok;
}

RingStatus FabricRingEndpoint::open_cxl_pinned(std::size_t capacity,
                                               std::uint64_t bus_addr) {
    kind_ = FabricTransportKind::CxlMemCoherent;
    const RingStatus st = cxl_ring_.allocate_pinned(capacity, bus_addr);
    if (st != RingStatus::Ok) {
        last_status_ = st;
        is_open_ = false;
        return st;
    }
    cxl_writer_ = std::make_unique<MockFpgaDmaProducer>(cxl_ring_.raw_base());
    is_open_ = cxl_writer_->is_ready();
    last_status_ = is_open_ ? RingStatus::Ok : RingStatus::MappingError;
    return last_status_;
}

RingStatus FabricRingEndpoint::open_rdma_producer(
    RdmaTransport* transport,
    RdmaRegionDescriptor remote_desc,
    FabricTransportKind kind) noexcept {
    if (transport == nullptr || remote_desc.capacity < 2 ||
        (remote_desc.capacity & (remote_desc.capacity - 1)) != 0 ||
        remote_desc.length_bytes < PcieCxlRing::required_bytes(remote_desc.capacity)) {
        last_status_ = RingStatus::InvalidArgument;
        return last_status_;
    }
    rdma_transport_ = transport;
    rdma_desc_ = remote_desc;
    kind_ = kind;
    rdma_head_ = 0;
    rdma_cached_tail_ = 0;

    std::uint64_t tail_val = 0;
    const RingStatus st = rdma_transport_->rdma_read(
        rdma_desc_, offsetof(PcieCxlRingHeader, host_tail), &tail_val, sizeof(tail_val));
    if (st != RingStatus::Ok) {
        last_status_ = st;
        is_open_ = false;
        return st;
    }
    rdma_cached_tail_ = tail_val;
    is_open_ = true;
    last_status_ = RingStatus::Ok;
    return RingStatus::Ok;
}

RingStatus FabricRingEndpoint::attach_rdma_consumer(
    void* local_pinned_base,
    std::size_t byte_length,
    std::size_t capacity,
    FabricTransportKind kind) noexcept {
    kind_ = kind;
    const RingStatus st = cxl_ring_.attach_mmio(
        local_pinned_base, byte_length, capacity, DmaTransportKind::CxlMemDax, 0, true);
    last_status_ = st;
    is_open_ = (st == RingStatus::Ok);
    return st;
}

void* FabricRingEndpoint::cxl_or_rdma_base() noexcept {
    return cxl_ring_.raw_base();
}

std::size_t FabricRingEndpoint::try_push_batch(std::span<const EventFrame> frames) noexcept {
    if (!is_open_ || frames.empty()) {
        return 0;
    }
    if (kind_ == FabricTransportKind::SharedMemory && shm_ring_) {
        return shm_ring_->try_push_batch(frames);
    }
    if (kind_ == FabricTransportKind::CxlMemCoherent && cxl_writer_) {
        return cxl_writer_->dma_write_burst(frames);
    }
    if ((kind_ == FabricTransportKind::RdmaRoceV2 ||
         kind_ == FabricTransportKind::RdmaInfiniBand) &&
        rdma_transport_ != nullptr) {
        const std::size_t cap = rdma_desc_.capacity;
        const std::size_t mask = cap - 1;
        std::uint64_t avail = cap - (rdma_head_ - rdma_cached_tail_);
        if (avail < frames.size()) {
            std::uint64_t remote_tail = rdma_cached_tail_;
            const RingStatus st = rdma_transport_->rdma_read(
                rdma_desc_, offsetof(PcieCxlRingHeader, host_tail),
                &remote_tail, sizeof(remote_tail));
            if (st != RingStatus::Ok) {
                last_status_ = st;
                return 0;
            }
            rdma_cached_tail_ = remote_tail;
            avail = cap - (rdma_head_ - rdma_cached_tail_);
        }
        const std::size_t to_push = std::min(frames.size(), static_cast<std::size_t>(avail));
        if (to_push == 0) {
            return 0;
        }
        const std::size_t start_slot = static_cast<std::size_t>(rdma_head_) & mask;
        const std::size_t first_chunk = std::min(to_push, cap - start_slot);
        const std::size_t second_chunk = to_push - first_chunk;

        const std::size_t slots_base_off = sizeof(PcieCxlRingHeader);
        RingStatus st = rdma_transport_->rdma_write(
            rdma_desc_,
            slots_base_off + start_slot * sizeof(EventFrame),
            frames.data(),
            first_chunk * sizeof(EventFrame));
        if (st != RingStatus::Ok) {
            last_status_ = st;
            return 0;
        }
        if (second_chunk > 0) {
            st = rdma_transport_->rdma_write(
                rdma_desc_,
                slots_base_off,
                frames.data() + first_chunk,
                second_chunk * sizeof(EventFrame));
            if (st != RingStatus::Ok) {
                last_status_ = st;
                return 0;
            }
        }
        rdma_head_ += to_push;
        st = rdma_transport_->rdma_write_imm_doorbell(rdma_desc_, rdma_head_);
        if (st != RingStatus::Ok) {
            last_status_ = st;
            return 0;
        }
        last_status_ = RingStatus::Ok;
        return to_push;
    }
    return 0;
}

std::size_t FabricRingEndpoint::try_pop_batch(std::span<EventFrame> out) noexcept {
    if (!is_open_ || out.empty()) {
        return 0;
    }
    if (kind_ == FabricTransportKind::SharedMemory && shm_ring_) {
        return shm_ring_->try_pop_batch(out);
    }
    return cxl_ring_.try_pop_batch(out);
}

std::size_t FabricRingEndpoint::size_approx() const noexcept {
    if (!is_open_) {
        return 0;
    }
    if (kind_ == FabricTransportKind::SharedMemory && shm_ring_) {
        return shm_ring_->size_approx();
    }
    if (cxl_ring_.is_open()) {
        return cxl_ring_.size_approx();
    }
    return static_cast<std::size_t>(rdma_head_ - rdma_cached_tail_);
}

} // namespace chronoring
