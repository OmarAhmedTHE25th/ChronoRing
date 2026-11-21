#pragma once

#include "chronoring/event_frame.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace chronoring {

template <typename T, std::size_t Capacity>
class SpscRingBuffer {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                  "SpscRingBuffer Capacity must be a power of 2 and >= 2");
    static_assert(std::is_move_constructible_v<T> || std::is_copy_constructible_v<T>,
                  "T must be copy or move constructible");

public:
    static constexpr std::size_t kCapacity = Capacity;
    static constexpr std::size_t kMask = Capacity - 1;

    SpscRingBuffer() = default;
    SpscRingBuffer(const SpscRingBuffer&) = delete;
    SpscRingBuffer& operator=(const SpscRingBuffer&) = delete;
    SpscRingBuffer(SpscRingBuffer&&) = delete;
    SpscRingBuffer& operator=(SpscRingBuffer&&) = delete;

    [[nodiscard]] bool try_push(const T& item) noexcept(std::is_nothrow_copy_assignable_v<T>) {
        const std::size_t head = prod_.head.load(std::memory_order_relaxed);
        if ((head - prod_.cached_tail) >= Capacity) {
            prod_.cached_tail = cons_.tail.load(std::memory_order_acquire);
            if ((head - prod_.cached_tail) >= Capacity) {
                return false;
            }
        }
        buffer_[head & kMask] = item;
        prod_.head.store(head + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool try_push(T&& item) noexcept(std::is_nothrow_move_assignable_v<T>) {
        const std::size_t head = prod_.head.load(std::memory_order_relaxed);
        if ((head - prod_.cached_tail) >= Capacity) {
            prod_.cached_tail = cons_.tail.load(std::memory_order_acquire);
            if ((head - prod_.cached_tail) >= Capacity) {
                return false;
            }
        }
        buffer_[head & kMask] = std::move(item);
        prod_.head.store(head + 1, std::memory_order_release);
        return true;
    }

    template <typename... Args>
    [[nodiscard]] bool try_emplace(Args&&... args) noexcept(
        std::is_nothrow_constructible_v<T, Args&&...> && std::is_nothrow_move_assignable_v<T>) {
        const std::size_t head = prod_.head.load(std::memory_order_relaxed);
        if ((head - prod_.cached_tail) >= Capacity) {
            prod_.cached_tail = cons_.tail.load(std::memory_order_acquire);
            if ((head - prod_.cached_tail) >= Capacity) {
                return false;
            }
        }
        buffer_[head & kMask] = T(std::forward<Args>(args)...);
        prod_.head.store(head + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool try_pop(T& out) noexcept(std::is_nothrow_move_assignable_v<T>) {
        const std::size_t tail = cons_.tail.load(std::memory_order_relaxed);
        if (cons_.cached_head == tail) {
            cons_.cached_head = prod_.head.load(std::memory_order_acquire);
            if (cons_.cached_head == tail) {
                return false;
            }
        }
        out = std::move(buffer_[tail & kMask]);
        cons_.tail.store(tail + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] std::size_t try_push_batch(const T* items, std::size_t count) noexcept(
        std::is_nothrow_copy_assignable_v<T>) {
        if (count == 0 || items == nullptr) {
            return 0;
        }
        const std::size_t head = prod_.head.load(std::memory_order_relaxed);
        std::size_t avail = Capacity - (head - prod_.cached_tail);
        if (avail < count) {
            prod_.cached_tail = cons_.tail.load(std::memory_order_acquire);
            avail = Capacity - (head - prod_.cached_tail);
        }
        const std::size_t to_push = std::min(count, avail);
        if (to_push == 0) {
            return 0;
        }
        for (std::size_t i = 0; i < to_push; ++i) {
            buffer_[(head + i) & kMask] = items[i];
        }
        prod_.head.store(head + to_push, std::memory_order_release);
        return to_push;
    }

    [[nodiscard]] std::size_t try_pop_batch(T* out, std::size_t max_count) noexcept(
        std::is_nothrow_move_assignable_v<T>) {
        if (max_count == 0 || out == nullptr) {
            return 0;
        }
        const std::size_t tail = cons_.tail.load(std::memory_order_relaxed);
        std::size_t avail = cons_.cached_head - tail;
        if (avail < max_count) {
            cons_.cached_head = prod_.head.load(std::memory_order_acquire);
            avail = cons_.cached_head - tail;
        }
        const std::size_t to_pop = std::min(max_count, avail);
        if (to_pop == 0) {
            return 0;
        }
        for (std::size_t i = 0; i < to_pop; ++i) {
            out[i] = std::move(buffer_[(tail + i) & kMask]);
        }
        cons_.tail.store(tail + to_pop, std::memory_order_release);
        return to_pop;
    }

    [[nodiscard]] std::size_t size_approx() const noexcept {
        const std::size_t tail = cons_.tail.load(std::memory_order_acquire);
        const std::size_t head = prod_.head.load(std::memory_order_acquire);
        const std::ptrdiff_t diff = static_cast<std::ptrdiff_t>(head - tail);
        if (diff <= 0) {
            return 0;
        }
        return std::min(static_cast<std::size_t>(diff), Capacity);
    }

    [[nodiscard]] bool empty() const noexcept {
        return size_approx() == 0;
    }

    [[nodiscard]] static constexpr std::size_t capacity() noexcept {
        return Capacity;
    }

private:
    struct alignas(kCacheLineSize) ProducerState {
        std::atomic<std::size_t> head{0};
        std::size_t cached_tail{0};
    } prod_;

    struct alignas(kCacheLineSize) ConsumerState {
        std::atomic<std::size_t> tail{0};
        std::size_t cached_head{0};
    } cons_;

    alignas(kCacheLineSize) std::array<T, Capacity> buffer_{};
};

} // namespace chronoring
