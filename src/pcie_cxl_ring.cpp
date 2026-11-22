#include "chronoring/pcie_cxl_ring.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <new>
#include <utility>

#ifdef _WIN32
#include <malloc.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace chronoring {

namespace {

constexpr bool is_power_of_two(std::size_t n) noexcept {
    return n >= 2 && (n & (n - 1)) == 0;
}

} // namespace

PcieCxlRing::~PcieCxlRing() {
    detach();
}

PcieCxlRing::PcieCxlRing(PcieCxlRing&& other) noexcept
    : mapping_base_(other.mapping_base_),
      mapped_bytes_(other.mapped_bytes_),
      header_(other.header_),
      slots_(other.slots_),
      capacity_(other.capacity_),
      mask_(other.mask_),
      cached_device_head_(other.cached_device_head_),
      local_host_tail_(other.local_host_tail_),
      ownership_(other.ownership_),
      fd_(other.fd_),
      last_status_(other.last_status_),
      last_error_(std::move(other.last_error_)) {
    other.mapping_base_ = nullptr;
    other.mapped_bytes_ = 0;
    other.header_ = nullptr;
    other.slots_ = nullptr;
    other.capacity_ = 0;
    other.mask_ = 0;
    other.cached_device_head_ = 0;
    other.local_host_tail_ = 0;
    other.ownership_ = Ownership::None;
    other.fd_ = -1;
}

PcieCxlRing& PcieCxlRing::operator=(PcieCxlRing&& other) noexcept {
    if (this != &other) {
        detach();
        mapping_base_ = other.mapping_base_;
        mapped_bytes_ = other.mapped_bytes_;
        header_ = other.header_;
        slots_ = other.slots_;
        capacity_ = other.capacity_;
        mask_ = other.mask_;
        cached_device_head_ = other.cached_device_head_;
        local_host_tail_ = other.local_host_tail_;
        ownership_ = other.ownership_;
        fd_ = other.fd_;
        last_status_ = other.last_status_;
        last_error_ = std::move(other.last_error_);

        other.mapping_base_ = nullptr;
        other.mapped_bytes_ = 0;
        other.header_ = nullptr;
        other.slots_ = nullptr;
        other.capacity_ = 0;
        other.mask_ = 0;
        other.cached_device_head_ = 0;
        other.local_host_tail_ = 0;
        other.ownership_ = Ownership::None;
        other.fd_ = -1;
    }
    return *this;
}

RingStatus PcieCxlRing::attach_mmio(void* base_addr,
                                    std::size_t byte_length,
                                    std::size_t capacity,
                                    DmaTransportKind kind,
                                    std::uint64_t dma_bus_addr,
                                    bool initialize_header) noexcept {
    detach();
    if (base_addr == nullptr || !is_power_of_two(capacity)) {
        last_status_ = RingStatus::InvalidArgument;
        last_error_ = "attach_mmio requires non-null base_addr and power-of-2 capacity >= 2";
        return last_status_;
    }
    if ((reinterpret_cast<std::uintptr_t>(base_addr) % kCacheLineSize) != 0) {
        last_status_ = RingStatus::InvalidArgument;
        last_error_ = "attach_mmio base_addr must be 64-byte cache-line aligned";
        return last_status_;
    }
    const std::size_t needed = required_bytes(capacity);
    if (byte_length < needed) {
        last_status_ = RingStatus::InvalidArgument;
        last_error_ = "attach_mmio byte_length smaller than required_bytes(capacity)";
        return last_status_;
    }

    auto* hdr = static_cast<PcieCxlRingHeader*>(base_addr);
    if (initialize_header) {
        std::memset(base_addr, 0, needed);
        hdr = new (base_addr) PcieCxlRingHeader{};
        hdr->magic = kPcieCxlMagic;
        hdr->version = kPcieCxlVersion;
        hdr->transport_kind = static_cast<std::uint16_t>(kind);
        hdr->capacity = static_cast<std::uint64_t>(capacity);
        hdr->element_size = sizeof(EventFrame);
        hdr->dma_bus_addr = dma_bus_addr;
        hdr->device_closed_flag.store(0, std::memory_order_relaxed);
        hdr->device_head.store(0, std::memory_order_relaxed);
        hdr->device_dropped_frames.store(0, std::memory_order_relaxed);
        hdr->host_tail.store(0, std::memory_order_relaxed);
        hdr->ready_flag.store(1, std::memory_order_release);
    } else {
        if (hdr->magic != kPcieCxlMagic || hdr->version != kPcieCxlVersion ||
            hdr->element_size != sizeof(EventFrame) ||
            hdr->capacity != static_cast<std::uint64_t>(capacity)) {
            last_status_ = RingStatus::InvalidArgument;
            last_error_ = "Existing PCIe/CXL header validation failed";
            return last_status_;
        }
    }

    mapping_base_ = base_addr;
    mapped_bytes_ = needed;
    header_ = hdr;
    slots_ = reinterpret_cast<EventFrame*>(static_cast<std::uint8_t*>(base_addr) +
                                           sizeof(PcieCxlRingHeader));
    capacity_ = capacity;
    mask_ = capacity - 1;
    local_host_tail_ = hdr->host_tail.load(std::memory_order_acquire);
    cached_device_head_ = hdr->device_head.load(std::memory_order_acquire);
    ownership_ = Ownership::ExternalMmio;
    last_status_ = RingStatus::Ok;
    last_error_.clear();
    return RingStatus::Ok;
}

RingStatus PcieCxlRing::open_device(const std::string& device_path,
                                    std::size_t capacity,
                                    DmaTransportKind kind,
                                    bool initialize_header) {
    detach();
    if (device_path.empty() || !is_power_of_two(capacity)) {
        last_status_ = RingStatus::InvalidArgument;
        last_error_ = "open_device requires non-empty path and power-of-2 capacity";
        return last_status_;
    }
#ifdef _WIN32
    (void)kind;
    (void)initialize_header;
    last_status_ = RingStatus::MappingError;
    last_error_ = "POSIX /dev/dax and sysfs BAR mmap are not supported on Win32";
    return last_status_;
#else
    const int fd = ::open(device_path.c_str(), O_RDWR);
    if (fd < 0) {
        last_status_ = RingStatus::IoError;
        last_error_ = std::string("open_device failed: ") + std::strerror(errno);
        return last_status_;
    }
    const std::size_t needed = required_bytes(capacity);
    void* view = ::mmap(nullptr, needed, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (view == MAP_FAILED) {
        const int err = errno;
        ::close(fd);
        last_status_ = RingStatus::MappingError;
        last_error_ = std::string("open_device mmap failed: ") + std::strerror(err);
        return last_status_;
    }
    const RingStatus st = attach_mmio(view, needed, capacity, kind, 0, initialize_header);
    if (st != RingStatus::Ok) {
        ::munmap(view, needed);
        ::close(fd);
        return st;
    }
    ownership_ = Ownership::DeviceMmap;
    fd_ = fd;
    return RingStatus::Ok;
#endif
}

RingStatus PcieCxlRing::allocate_pinned(std::size_t capacity,
                                        std::uint64_t dma_bus_addr) noexcept {
    detach();
    if (!is_power_of_two(capacity)) {
        last_status_ = RingStatus::InvalidArgument;
        last_error_ = "allocate_pinned requires power-of-2 capacity >= 2";
        return last_status_;
    }
    const std::size_t needed = required_bytes(capacity);
#ifdef _WIN32
    void* buf = _aligned_malloc(needed, kCacheLineSize);
    if (buf == nullptr) {
        last_status_ = RingStatus::MappingError;
        last_error_ = "_aligned_malloc failed for PcieCxlRing";
        return last_status_;
    }
#else
    void* buf = ::mmap(nullptr, needed, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (buf == MAP_FAILED) {
        last_status_ = RingStatus::MappingError;
        last_error_ = std::string("allocate_pinned mmap failed: ") + std::strerror(errno);
        return last_status_;
    }
    // Best-effort memory pinning; unprivileged containers may refuse RLIMIT_MEMLOCK.
    (void)::mlock(buf, needed);
#endif

    const RingStatus st = attach_mmio(
        buf, needed, capacity, DmaTransportKind::HostPinned, dma_bus_addr, true);
    if (st != RingStatus::Ok) {
#ifdef _WIN32
        _aligned_free(buf);
#else
        ::munmap(buf, needed);
#endif
        return st;
    }
    ownership_ = Ownership::HostPinnedAlloc;
    return RingStatus::Ok;
}

void PcieCxlRing::detach() noexcept {
    if (mapping_base_ != nullptr) {
        if (ownership_ == Ownership::HostPinnedAlloc) {
#ifdef _WIN32
            _aligned_free(mapping_base_);
#else
            (void)::munlock(mapping_base_, mapped_bytes_);
            ::munmap(mapping_base_, mapped_bytes_);
#endif
        } else if (ownership_ == Ownership::DeviceMmap) {
#ifndef _WIN32
            ::munmap(mapping_base_, mapped_bytes_);
            if (fd_ >= 0) {
                ::close(fd_);
                fd_ = -1;
            }
#endif
        }
    }
    mapping_base_ = nullptr;
    mapped_bytes_ = 0;
    header_ = nullptr;
    slots_ = nullptr;
    capacity_ = 0;
    mask_ = 0;
    cached_device_head_ = 0;
    local_host_tail_ = 0;
    ownership_ = Ownership::None;
    fd_ = -1;
}

bool PcieCxlRing::is_device_closed() const noexcept {
    if (header_ == nullptr) {
        return true;
    }
    return header_->device_closed_flag.load(std::memory_order_acquire) != 0;
}

DmaTransportKind PcieCxlRing::transport_kind() const noexcept {
    if (header_ == nullptr) {
        return DmaTransportKind::PcieBarMmio;
    }
    return static_cast<DmaTransportKind>(header_->transport_kind);
}

std::uint64_t PcieCxlRing::dma_bus_addr() const noexcept {
    return (header_ != nullptr) ? header_->dma_bus_addr : 0ULL;
}

std::uint64_t PcieCxlRing::device_dropped_frames() const noexcept {
    if (header_ == nullptr) {
        return 0ULL;
    }
    return header_->device_dropped_frames.load(std::memory_order_acquire);
}

const EventFrame* PcieCxlRing::try_peek() noexcept {
    if (header_ == nullptr) {
        return nullptr;
    }
    const std::uint64_t tail = local_host_tail_;
    if (tail == cached_device_head_) {
        cached_device_head_ = header_->device_head.load(std::memory_order_acquire);
        if (tail == cached_device_head_) {
            return nullptr;
        }
    }
    return &slots_[static_cast<std::size_t>(tail) & mask_];
}

void PcieCxlRing::consume_pop() noexcept {
    if (header_ == nullptr) {
        return;
    }
    const std::uint64_t next_tail = local_host_tail_ + 1;
    local_host_tail_ = next_tail;
    header_->host_tail.store(next_tail, std::memory_order_release);
}

RingStatus PcieCxlRing::try_pop_status(EventFrame& out) noexcept {
    if (header_ == nullptr) {
        return RingStatus::Closed;
    }
    const EventFrame* ptr = try_peek();
    if (ptr == nullptr) {
        return is_device_closed() ? RingStatus::Closed : RingStatus::Empty;
    }
    out = *ptr;
    consume_pop();
    return RingStatus::Ok;
}

std::size_t PcieCxlRing::try_pop_batch(EventFrame* out,
                                       std::size_t max_count) noexcept {
    if (header_ == nullptr || out == nullptr || max_count == 0) {
        return 0;
    }
    const std::uint64_t tail = local_host_tail_;
    std::uint64_t avail = cached_device_head_ - tail;
    if (avail < max_count) {
        cached_device_head_ = header_->device_head.load(std::memory_order_acquire);
        avail = cached_device_head_ - tail;
    }
    const std::size_t to_pop = std::min(max_count, static_cast<std::size_t>(avail));
    if (to_pop == 0) {
        return 0;
    }
    for (std::size_t i = 0; i < to_pop; ++i) {
        out[i] = slots_[static_cast<std::size_t>(tail + i) & mask_];
    }
    local_host_tail_ = tail + to_pop;
    header_->host_tail.store(local_host_tail_, std::memory_order_release);
    return to_pop;
}

std::size_t PcieCxlRing::size_approx() const noexcept {
    if (header_ == nullptr) {
        return 0;
    }
    const std::uint64_t head = header_->device_head.load(std::memory_order_acquire);
    const std::uint64_t tail = header_->host_tail.load(std::memory_order_acquire);
    if (head <= tail) {
        return 0;
    }
    return std::min(static_cast<std::size_t>(head - tail), capacity_);
}

// ============================================================================
// MockFpgaDmaProducer
// ============================================================================

MockFpgaDmaProducer::MockFpgaDmaProducer(void* dma_base) noexcept {
    if (dma_base == nullptr) {
        return;
    }
    auto* hdr = static_cast<PcieCxlRingHeader*>(dma_base);
    if (hdr->magic != kPcieCxlMagic || !is_power_of_two(static_cast<std::size_t>(hdr->capacity))) {
        return;
    }
    header_ = hdr;
    capacity_ = static_cast<std::size_t>(hdr->capacity);
    mask_ = capacity_ - 1;
    slots_ = reinterpret_cast<EventFrame*>(static_cast<std::uint8_t*>(dma_base) +
                                           sizeof(PcieCxlRingHeader));
    fpga_head_ = hdr->device_head.load(std::memory_order_acquire);
    cached_host_tail_ = hdr->host_tail.load(std::memory_order_acquire);
}

bool MockFpgaDmaProducer::is_ready() const noexcept {
    return header_ != nullptr && header_->ready_flag.load(std::memory_order_acquire) != 0;
}

std::size_t MockFpgaDmaProducer::dma_write_burst(const EventFrame* frames,
                                                 std::size_t count) noexcept {
    if (!is_ready() || frames == nullptr || count == 0) {
        return 0;
    }
    const std::uint64_t head = fpga_head_;
    std::uint64_t avail = capacity_ - (head - cached_host_tail_);
    if (avail < count) {
        cached_host_tail_ = header_->host_tail.load(std::memory_order_acquire);
        avail = capacity_ - (head - cached_host_tail_);
    }
    const std::size_t to_write = std::min(count, static_cast<std::size_t>(avail));
    if (to_write < count) {
        header_->device_dropped_frames.fetch_add(
            static_cast<std::uint64_t>(count - to_write), std::memory_order_relaxed);
    }
    if (to_write == 0) {
        return 0;
    }
    for (std::size_t i = 0; i < to_write; ++i) {
        slots_[static_cast<std::size_t>(head + i) & mask_] = frames[i];
    }
    fpga_head_ = head + to_write;
    header_->device_head.store(fpga_head_, std::memory_order_release);
    return to_write;
}

void MockFpgaDmaProducer::signal_device_close() noexcept {
    if (header_ != nullptr) {
        header_->device_closed_flag.store(1, std::memory_order_release);
    }
}

} // namespace chronoring
