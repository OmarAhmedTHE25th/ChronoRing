#include "chronoring/shm_ring.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <new>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace chronoring {

namespace {

constexpr bool is_power_of_two(std::size_t n) noexcept {
    return n >= 2 && (n & (n - 1)) == 0;
}

} // namespace

std::string ShmRingBuffer::normalize_shm_name(const std::string& name) {
    std::string clean;
    clean.reserve(name.size() + 8);
#ifdef _WIN32
    clean = "Local\\chronoring_";
    for (char ch : name) {
        if (ch == '/' || ch == '\\') {
            if (!clean.empty() && clean.back() != '_') {
                clean.push_back('_');
            }
        } else {
            clean.push_back(ch);
        }
    }
#else
    clean.push_back('/');
    for (std::size_t i = 0; i < name.size(); ++i) {
        const char ch = name[i];
        if (i == 0 && ch == '/') {
            continue;
        }
        clean.push_back(ch == '/' ? '_' : ch);
    }
    if (clean.size() == 1) {
        clean += "chronoring_default";
    }
#endif
    return clean;
}

ShmRingBuffer::ShmRingBuffer(std::string name, ShmMode mode, std::size_t capacity,
                             bool unlink_on_destroy)
    : name_(std::move(name)),
      normalized_name_(normalize_shm_name(name_)),
      unlink_on_destroy_(unlink_on_destroy) {
    if (mode == ShmMode::Create && !is_power_of_two(capacity)) {
        last_error_ = "ShmRingBuffer capacity must be a power of 2 and >= 2";
        return;
    }

#ifdef _WIN32
    if (mode == ShmMode::Create) {
        const std::size_t total_bytes =
            sizeof(ShmRingHeader) + capacity * sizeof(EventFrame);
        const DWORD size_high =
            static_cast<DWORD>((static_cast<std::uint64_t>(total_bytes) >> 32) & 0xFFFFFFFFu);
        const DWORD size_low =
            static_cast<DWORD>(static_cast<std::uint64_t>(total_bytes) & 0xFFFFFFFFu);
        HANDLE handle = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                           size_high, size_low, normalized_name_.c_str());
        if (handle == nullptr) {
            last_error_ = "CreateFileMappingA failed";
            return;
        }
        void* view = MapViewOfFile(handle, FILE_MAP_ALL_ACCESS, 0, 0, total_bytes);
        if (view == nullptr) {
            CloseHandle(handle);
            last_error_ = "MapViewOfFile failed";
            return;
        }
        std::memset(view, 0, total_bytes);
        auto* hdr = new (view) ShmRingHeader{};
        hdr->capacity = static_cast<std::uint64_t>(capacity);
        hdr->element_size = sizeof(EventFrame);
        hdr->head.store(0, std::memory_order_relaxed);
        hdr->tail.store(0, std::memory_order_relaxed);
        hdr->closed_flag.store(0, std::memory_order_relaxed);
        hdr->ready_flag.store(1, std::memory_order_release);

        file_mapping_handle_ = handle;
        mapping_base_ = view;
        mapped_bytes_ = total_bytes;
        header_ = hdr;
        slots_ = reinterpret_cast<EventFrame*>(static_cast<std::uint8_t*>(view) +
                                               sizeof(ShmRingHeader));
        capacity_ = capacity;
        mask_ = capacity - 1;
    } else {
        HANDLE handle =
            OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, normalized_name_.c_str());
        if (handle == nullptr) {
            last_error_ = "OpenFileMappingA failed";
            return;
        }
        void* view = MapViewOfFile(handle, FILE_MAP_ALL_ACCESS, 0, 0, 0);
        if (view == nullptr) {
            CloseHandle(handle);
            last_error_ = "MapViewOfFile (Open) failed";
            return;
        }
        auto* hdr = static_cast<ShmRingHeader*>(view);
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (hdr->ready_flag.load(std::memory_order_acquire) == 0) {
            if (std::chrono::steady_clock::now() >= deadline) {
                UnmapViewOfFile(view);
                CloseHandle(handle);
                last_error_ = "Timed out waiting for shared-memory header ready_flag";
                return;
            }
            std::this_thread::yield();
        }
        if (hdr->magic != kShmRingMagic || hdr->version != kShmRingVersion ||
            hdr->element_size != sizeof(EventFrame) ||
            !is_power_of_two(static_cast<std::size_t>(hdr->capacity))) {
            UnmapViewOfFile(view);
            CloseHandle(handle);
            last_error_ = "Invalid shared-memory ring header";
            return;
        }
        const std::size_t cap = static_cast<std::size_t>(hdr->capacity);
        const std::size_t total_bytes = sizeof(ShmRingHeader) + cap * sizeof(EventFrame);
        file_mapping_handle_ = handle;
        mapping_base_ = view;
        mapped_bytes_ = total_bytes;
        header_ = hdr;
        slots_ = reinterpret_cast<EventFrame*>(static_cast<std::uint8_t*>(view) +
                                               sizeof(ShmRingHeader));
        capacity_ = cap;
        mask_ = cap - 1;
    }
#else
    if (mode == ShmMode::Create) {
        ::shm_unlink(normalized_name_.c_str());
        const int fd =
            ::shm_open(normalized_name_.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
        if (fd < 0) {
            last_error_ = std::string("shm_open(Create) failed: ") + std::strerror(errno);
            return;
        }
        const std::size_t total_bytes =
            sizeof(ShmRingHeader) + capacity * sizeof(EventFrame);
        if (::ftruncate(fd, static_cast<off_t>(total_bytes)) != 0) {
            const int err = errno;
            ::close(fd);
            ::shm_unlink(normalized_name_.c_str());
            last_error_ = std::string("ftruncate failed: ") + std::strerror(err);
            return;
        }
        void* view = ::mmap(nullptr, total_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (view == MAP_FAILED) {
            const int err = errno;
            ::close(fd);
            ::shm_unlink(normalized_name_.c_str());
            last_error_ = std::string("mmap(Create) failed: ") + std::strerror(err);
            return;
        }
        std::memset(view, 0, total_bytes);
        auto* hdr = new (view) ShmRingHeader{};
        hdr->capacity = static_cast<std::uint64_t>(capacity);
        hdr->element_size = sizeof(EventFrame);
        hdr->head.store(0, std::memory_order_relaxed);
        hdr->tail.store(0, std::memory_order_relaxed);
        hdr->closed_flag.store(0, std::memory_order_relaxed);
        hdr->ready_flag.store(1, std::memory_order_release);

        shm_fd_ = fd;
        mapping_base_ = view;
        mapped_bytes_ = total_bytes;
        header_ = hdr;
        slots_ = reinterpret_cast<EventFrame*>(static_cast<std::uint8_t*>(view) +
                                               sizeof(ShmRingHeader));
        capacity_ = capacity;
        mask_ = capacity - 1;
    } else {
        const int fd = ::shm_open(normalized_name_.c_str(), O_RDWR, 0600);
        if (fd < 0) {
            last_error_ = std::string("shm_open(Open) failed: ") + std::strerror(errno);
            return;
        }
        void* hdr_view =
            ::mmap(nullptr, sizeof(ShmRingHeader), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (hdr_view == MAP_FAILED) {
            const int err = errno;
            ::close(fd);
            last_error_ = std::string("mmap(Header) failed: ") + std::strerror(err);
            return;
        }
        auto* probe_hdr = static_cast<ShmRingHeader*>(hdr_view);
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (probe_hdr->ready_flag.load(std::memory_order_acquire) == 0) {
            if (std::chrono::steady_clock::now() >= deadline) {
                ::munmap(hdr_view, sizeof(ShmRingHeader));
                ::close(fd);
                last_error_ = "Timed out waiting for shared-memory header ready_flag";
                return;
            }
            std::this_thread::yield();
        }
        const std::uint32_t magic = probe_hdr->magic;
        const std::uint16_t version = probe_hdr->version;
        const std::uint64_t elem_size = probe_hdr->element_size;
        const std::size_t cap = static_cast<std::size_t>(probe_hdr->capacity);
        ::munmap(hdr_view, sizeof(ShmRingHeader));

        if (magic != kShmRingMagic || version != kShmRingVersion ||
            elem_size != sizeof(EventFrame) || !is_power_of_two(cap)) {
            ::close(fd);
            last_error_ = "Invalid shared-memory ring header";
            return;
        }

        const std::size_t total_bytes = sizeof(ShmRingHeader) + cap * sizeof(EventFrame);
        void* view = ::mmap(nullptr, total_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (view == MAP_FAILED) {
            const int err = errno;
            ::close(fd);
            last_error_ = std::string("mmap(Open) failed: ") + std::strerror(err);
            return;
        }
        shm_fd_ = fd;
        mapping_base_ = view;
        mapped_bytes_ = total_bytes;
        header_ = static_cast<ShmRingHeader*>(view);
        slots_ = reinterpret_cast<EventFrame*>(static_cast<std::uint8_t*>(view) +
                                               sizeof(ShmRingHeader));
        capacity_ = cap;
        mask_ = cap - 1;
    }
#endif
}

ShmRingBuffer::~ShmRingBuffer() {
    unmap();
    if (unlink_on_destroy_ && !normalized_name_.empty()) {
        unlink(name_);
    }
}

ShmRingBuffer::ShmRingBuffer(ShmRingBuffer&& other) noexcept
    : name_(std::move(other.name_)),
      normalized_name_(std::move(other.normalized_name_)),
      capacity_(other.capacity_),
      mask_(other.mask_),
      mapped_bytes_(other.mapped_bytes_),
      unlink_on_destroy_(other.unlink_on_destroy_),
      mapping_base_(other.mapping_base_),
      header_(other.header_),
      slots_(other.slots_),
      cached_tail_(other.cached_tail_),
      cached_head_(other.cached_head_),
      last_error_(std::move(other.last_error_)),
#ifdef _WIN32
      file_mapping_handle_(other.file_mapping_handle_)
#else
      shm_fd_(other.shm_fd_)
#endif
{
    other.capacity_ = 0;
    other.mask_ = 0;
    other.mapped_bytes_ = 0;
    other.unlink_on_destroy_ = false;
    other.mapping_base_ = nullptr;
    other.header_ = nullptr;
    other.slots_ = nullptr;
    other.cached_tail_ = 0;
    other.cached_head_ = 0;
#ifdef _WIN32
    other.file_mapping_handle_ = nullptr;
#else
    other.shm_fd_ = -1;
#endif
}

ShmRingBuffer& ShmRingBuffer::operator=(ShmRingBuffer&& other) noexcept {
    if (this != &other) {
        unmap();
        if (unlink_on_destroy_ && !normalized_name_.empty()) {
            unlink(name_);
        }
        name_ = std::move(other.name_);
        normalized_name_ = std::move(other.normalized_name_);
        capacity_ = other.capacity_;
        mask_ = other.mask_;
        mapped_bytes_ = other.mapped_bytes_;
        unlink_on_destroy_ = other.unlink_on_destroy_;
        mapping_base_ = other.mapping_base_;
        header_ = other.header_;
        slots_ = other.slots_;
        cached_tail_ = other.cached_tail_;
        cached_head_ = other.cached_head_;
        last_error_ = std::move(other.last_error_);
#ifdef _WIN32
        file_mapping_handle_ = other.file_mapping_handle_;
        other.file_mapping_handle_ = nullptr;
#else
        shm_fd_ = other.shm_fd_;
        other.shm_fd_ = -1;
#endif
        other.capacity_ = 0;
        other.mask_ = 0;
        other.mapped_bytes_ = 0;
        other.unlink_on_destroy_ = false;
        other.mapping_base_ = nullptr;
        other.header_ = nullptr;
        other.slots_ = nullptr;
        other.cached_tail_ = 0;
        other.cached_head_ = 0;
    }
    return *this;
}

void ShmRingBuffer::unmap() noexcept {
#ifdef _WIN32
    if (mapping_base_ != nullptr) {
        UnmapViewOfFile(mapping_base_);
        mapping_base_ = nullptr;
    }
    if (file_mapping_handle_ != nullptr) {
        CloseHandle(file_mapping_handle_);
        file_mapping_handle_ = nullptr;
    }
#else
    if (mapping_base_ != nullptr) {
        ::munmap(mapping_base_, mapped_bytes_);
        mapping_base_ = nullptr;
    }
    if (shm_fd_ >= 0) {
        ::close(shm_fd_);
        shm_fd_ = -1;
    }
#endif
    header_ = nullptr;
    slots_ = nullptr;
    mapped_bytes_ = 0;
}

void ShmRingBuffer::close() noexcept {
    if (header_ != nullptr) {
        header_->closed_flag.store(1, std::memory_order_release);
    }
}

bool ShmRingBuffer::is_closed() const noexcept {
    if (header_ == nullptr) {
        return true;
    }
    return header_->closed_flag.load(std::memory_order_acquire) != 0;
}

bool ShmRingBuffer::try_push(const EventFrame& frame) noexcept {
    if (header_ == nullptr) {
        return false;
    }
    const std::uint64_t head = header_->head.load(std::memory_order_relaxed);
    if ((head - cached_tail_) >= capacity_) {
        cached_tail_ = header_->tail.load(std::memory_order_acquire);
        if ((head - cached_tail_) >= capacity_) {
            return false;
        }
    }
    slots_[static_cast<std::size_t>(head) & mask_] = frame;
    header_->head.store(head + 1, std::memory_order_release);
    return true;
}

bool ShmRingBuffer::try_pop(EventFrame& out) noexcept {
    if (header_ == nullptr) {
        return false;
    }
    const std::uint64_t tail = header_->tail.load(std::memory_order_relaxed);
    if (cached_head_ == tail) {
        cached_head_ = header_->head.load(std::memory_order_acquire);
        if (cached_head_ == tail) {
            return false;
        }
    }
    out = slots_[static_cast<std::size_t>(tail) & mask_];
    header_->tail.store(tail + 1, std::memory_order_release);
    return true;
}

std::size_t ShmRingBuffer::try_push_batch(const EventFrame* frames,
                                          std::size_t count) noexcept {
    if (header_ == nullptr || frames == nullptr || count == 0) {
        return 0;
    }
    const std::uint64_t head = header_->head.load(std::memory_order_relaxed);
    std::uint64_t avail = capacity_ - (head - cached_tail_);
    if (avail < count) {
        cached_tail_ = header_->tail.load(std::memory_order_acquire);
        avail = capacity_ - (head - cached_tail_);
    }
    const std::size_t to_push =
        std::min(count, static_cast<std::size_t>(avail));
    if (to_push == 0) {
        return 0;
    }
    for (std::size_t i = 0; i < to_push; ++i) {
        slots_[static_cast<std::size_t>(head + i) & mask_] = frames[i];
    }
    header_->head.store(head + to_push, std::memory_order_release);
    return to_push;
}

std::size_t ShmRingBuffer::try_pop_batch(EventFrame* out,
                                         std::size_t max_count) noexcept {
    if (header_ == nullptr || out == nullptr || max_count == 0) {
        return 0;
    }
    const std::uint64_t tail = header_->tail.load(std::memory_order_relaxed);
    std::uint64_t avail = cached_head_ - tail;
    if (avail < max_count) {
        cached_head_ = header_->head.load(std::memory_order_acquire);
        avail = cached_head_ - tail;
    }
    const std::size_t to_pop =
        std::min(max_count, static_cast<std::size_t>(avail));
    if (to_pop == 0) {
        return 0;
    }
    for (std::size_t i = 0; i < to_pop; ++i) {
        out[i] = slots_[static_cast<std::size_t>(tail + i) & mask_];
    }
    header_->tail.store(tail + to_pop, std::memory_order_release);
    return to_pop;
}

std::size_t ShmRingBuffer::size_approx() const noexcept {
    if (header_ == nullptr) {
        return 0;
    }
    const std::uint64_t tail = header_->tail.load(std::memory_order_acquire);
    const std::uint64_t head = header_->head.load(std::memory_order_acquire);
    if (head <= tail) {
        return 0;
    }
    return std::min(static_cast<std::size_t>(head - tail), capacity_);
}

bool ShmRingBuffer::unlink(const std::string& name) noexcept {
#ifdef _WIN32
    (void)name;
    return true;
#else
    const std::string normalized = normalize_shm_name(name);
    return ::shm_unlink(normalized.c_str()) == 0;
#endif
}

} // namespace chronoring
