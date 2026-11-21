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
class MpscRingBuffer {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                  "MpscRingBuffer Capacity must be a power of 2 and >= 2");
    static_assert(std::is_move_constructible_v<T> || std::is_copy_constructible_v<T>,
                  "T must be copy or move constructible");

public:
    static constexpr std::size_t kCapacity = Capacity;
    static constexpr std::size_t kMask = Capacity - 1;

    MpscRingBuffer() noexcept {
        for (std::size_t i = 0; i < Capacity; ++i) {
            slots_[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    MpscRingBuffer(const MpscRingBuffer&) = delete;
    MpscRingBuffer& operator=(const MpscRingBuffer&) = delete;
    MpscRingBuffer(MpscRingBuffer&&) = delete;
    MpscRingBuffer& operator=(MpscRingBuffer&&) = delete;

    [[nodiscard]] bool try_push(const T& item) noexcept(std::is_nothrow_copy_assignable_v<T>) {
        return try_emplace(item);
    }

    [[nodiscard]] bool try_push(T&& item) noexcept(std::is_nothrow_move_assignable_v<T>) {
        return try_emplace(std::move(item));
    }

    template <typename... Args>
    [[nodiscard]] bool try_emplace(Args&&... args) noexcept(
        std::is_nothrow_constructible_v<T, Args&&...> && std::is_nothrow_move_assignable_v<T>) {
        std::size_t pos = enqueue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            Slot& slot = slots_[pos & kMask];
            const std::size_t seq = slot.sequence.load(std::memory_order_acquire);
            const std::ptrdiff_t diff =
                static_cast<std::ptrdiff_t>(seq) - static_cast<std::ptrdiff_t>(pos);
            if (diff == 0) {
                if (enqueue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    slot.data = T(std::forward<Args>(args)...);
                    slot.sequence.store(pos + 1, std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) {
                return false;
            } else {
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }
    }

    [[nodiscard]] bool try_pop(T& out) noexcept(std::is_nothrow_move_assignable_v<T>) {
        const std::size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
        Slot& slot = slots_[pos & kMask];
        const std::size_t seq = slot.sequence.load(std::memory_order_acquire);
        const std::ptrdiff_t diff =
            static_cast<std::ptrdiff_t>(seq) - static_cast<std::ptrdiff_t>(pos + 1);
        if (diff < 0) {
            return false;
        }
        out = std::move(slot.data);
        slot.sequence.store(pos + Capacity, std::memory_order_release);
        dequeue_pos_.store(pos + 1, std::memory_order_relaxed);
        return true;
    }

    [[nodiscard]] std::size_t try_pop_batch(T* out, std::size_t max_count) noexcept(
        std::is_nothrow_move_assignable_v<T>) {
        if (max_count == 0 || out == nullptr) {
            return 0;
        }
        std::size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
        std::size_t popped = 0;
        while (popped < max_count) {
            Slot& slot = slots_[pos & kMask];
            const std::size_t seq = slot.sequence.load(std::memory_order_acquire);
            const std::ptrdiff_t diff =
                static_cast<std::ptrdiff_t>(seq) - static_cast<std::ptrdiff_t>(pos + 1);
            if (diff < 0) {
                break;
            }
            out[popped] = std::move(slot.data);
            slot.sequence.store(pos + Capacity, std::memory_order_release);
            ++pos;
            ++popped;
        }
        if (popped > 0) {
            dequeue_pos_.store(pos, std::memory_order_relaxed);
        }
        return popped;
    }

    [[nodiscard]] std::size_t size_approx() const noexcept {
        const std::size_t deq = dequeue_pos_.load(std::memory_order_acquire);
        const std::size_t enq = enqueue_pos_.load(std::memory_order_acquire);
        const std::ptrdiff_t diff = static_cast<std::ptrdiff_t>(enq - deq);
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
    struct alignas(kCacheLineSize) Slot {
        std::atomic<std::size_t> sequence{0};
        T data{};
    };

    alignas(kCacheLineSize) std::atomic<std::size_t> enqueue_pos_{0};
    alignas(kCacheLineSize) std::atomic<std::size_t> dequeue_pos_{0};
    alignas(kCacheLineSize) std::array<Slot, Capacity> slots_{};
};

} // namespace chronoring
