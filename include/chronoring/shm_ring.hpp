#pragma once

#include "chronoring/event_frame.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace chronoring {

inline constexpr std::uint32_t kShmRingMagic = 0x53484D52u; // "SHMR"
inline constexpr std::uint16_t kShmRingVersion = 1u;

enum class ShmMode {
    Create,
    Open
};

struct alignas(kCacheLineSize) ShmRingHeader {
    std::uint32_t magic{kShmRingMagic};
    std::uint16_t version{kShmRingVersion};
    std::uint16_t flags{0};
    std::uint64_t capacity{0};
    std::uint64_t element_size{sizeof(EventFrame)};
    std::atomic<std::uint32_t> ready_flag{0};
    std::atomic<std::uint32_t> closed_flag{0};
    std::uint8_t pad0[32]{};

    alignas(kCacheLineSize) std::atomic<std::uint64_t> head{0};
    std::uint64_t pad1[7]{};

    alignas(kCacheLineSize) std::atomic<std::uint64_t> tail{0};
    std::uint64_t pad2[7]{};
};

static_assert(sizeof(ShmRingHeader) == 192, "ShmRingHeader must occupy 3 cache lines");
static_assert(alignof(ShmRingHeader) == 64, "ShmRingHeader must be 64-byte aligned");
static_assert(std::atomic<std::uint64_t>::is_always_lock_free, "64-bit atomics must be lock-free for IPC");
static_assert(std::atomic<std::uint32_t>::is_always_lock_free, "32-bit atomics must be lock-free for IPC");

class ShmRingBuffer {
public:
    ShmRingBuffer() noexcept = default;
    ShmRingBuffer(std::string name, ShmMode mode, std::size_t capacity = 4096,
                  bool unlink_on_destroy = false);
    ~ShmRingBuffer();

    ShmRingBuffer(const ShmRingBuffer&) = delete;
    ShmRingBuffer& operator=(const ShmRingBuffer&) = delete;
    ShmRingBuffer(ShmRingBuffer&& other) noexcept;
    ShmRingBuffer& operator=(ShmRingBuffer&& other) noexcept;

    [[nodiscard]] bool is_open() const noexcept { return header_ != nullptr; }
    [[nodiscard]] bool is_closed() const noexcept;
    void close() noexcept;
    void unmap() noexcept;

    [[nodiscard]] bool try_push(const EventFrame& frame) noexcept;
    [[nodiscard]] bool try_pop(EventFrame& out) noexcept;
    [[nodiscard]] std::size_t try_push_batch(const EventFrame* frames, std::size_t count) noexcept;
    [[nodiscard]] std::size_t try_pop_batch(EventFrame* out, std::size_t max_count) noexcept;

    [[nodiscard]] std::size_t size_approx() const noexcept;
    [[nodiscard]] bool empty() const noexcept { return size_approx() == 0; }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] const std::string& last_error() const noexcept { return last_error_; }

    static bool unlink(const std::string& name) noexcept;

private:
    static std::string normalize_shm_name(const std::string& name);

    std::string name_{};
    std::string normalized_name_{};
    std::size_t capacity_{0};
    std::size_t mask_{0};
    std::size_t mapped_bytes_{0};
    bool unlink_on_destroy_{false};

    void* mapping_base_{nullptr};
    ShmRingHeader* header_{nullptr};
    EventFrame* slots_{nullptr};

    std::uint64_t cached_tail_{0};
    std::uint64_t cached_head_{0};
    std::string last_error_{};

#ifdef _WIN32
    void* file_mapping_handle_{nullptr};
#else
    int shm_fd_{-1};
#endif
};

} // namespace chronoring
