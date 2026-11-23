#pragma once

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4324) // structure was padded due to alignment specifier
#endif

#include "chronoring/event_frame.hpp"
#include "chronoring/status.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <thread>
#include <type_traits>
#include <utility>

namespace chronoring {

// Bounded Multi-Producer Single-Consumer ring buffer using a Vyukov per-slot
// sequence protocol with lock-free CAS slot reservation on `enqueue_pos_`.
//
// Progress & memory-ordering contract:
//  - Producers claim a ticket via lock-free CAS on `enqueue_pos_` after
//    constructing/staging the value into a local object, then perform a `noexcept`
//    move-assignment into `slot.data` and publish via `slot.sequence.store(pos + 1,
//    std::memory_order_release)`.
//  - Note on bounded Vyukov MPSC progress: because slots are published in sequence
//    order, a producer thread preempted or stalled mid-write between claiming a
//    ticket and publishing `slot.sequence` temporarily blocks consumer progress on
//    that specific slot until it resumes. Strict wait-free progress holds on SPSC
//    channels (`SpscRingBuffer` and `SpscFanOut`).
template <typename T, std::size_t Capacity>
    requires (Capacity >= 2 && (Capacity & (Capacity - 1)) == 0) &&
             std::default_initializable<T> &&
             std::movable<T> &&
             std::is_nothrow_destructible_v<T> &&
             std::is_nothrow_move_assignable_v<T>
class MpscRingBuffer {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                  "MpscRingBuffer Capacity must be a power of 2 and >= 2");
    static_assert(std::is_nothrow_move_assignable_v<T> && std::is_nothrow_destructible_v<T>,
                  "T must have noexcept move assignment and destructor");

public:
    static constexpr std::size_t kCapacity = Capacity;
    static constexpr std::size_t kMask = Capacity - 1;
    static constexpr bool kSlotResetNothrow =
        std::is_trivially_destructible_v<T> ||
        (std::is_nothrow_default_constructible_v<T> && std::is_nothrow_move_assignable_v<T>);

    MpscRingBuffer() noexcept {
        for (std::size_t i = 0; i < Capacity; ++i) {
            slots_[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    MpscRingBuffer(const MpscRingBuffer&) = delete;
    MpscRingBuffer& operator=(const MpscRingBuffer&) = delete;
    MpscRingBuffer(MpscRingBuffer&&) = delete;
    MpscRingBuffer& operator=(MpscRingBuffer&&) = delete;

    [[nodiscard]] RingStatus try_push_status(const T& item) noexcept(
        std::is_nothrow_copy_constructible_v<T>)
        requires std::copy_constructible<T> {
        return try_push(item) ? RingStatus::Ok : RingStatus::Full;
    }

    [[nodiscard]] RingStatus try_pop_status(T& out) noexcept(kSlotResetNothrow) {
        return try_pop(out) ? RingStatus::Ok : RingStatus::Empty;
    }

    [[nodiscard]] bool try_push(const T& item) noexcept(
        std::is_nothrow_copy_constructible_v<T>)
        requires std::copy_constructible<T> {
        // Construct copy before claiming a slot so any copy exception leaves
        // `enqueue_pos_` and `slot.sequence` intact.
        T copy(item);
        return try_push(std::move(copy));
    }

    [[nodiscard]] bool try_push(T&& item) noexcept {
        std::size_t pos = enqueue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            Slot& slot = slots_[pos & kMask];
            const std::size_t seq = slot.sequence.load(std::memory_order_acquire);
            const std::ptrdiff_t diff =
                static_cast<std::ptrdiff_t>(seq) - static_cast<std::ptrdiff_t>(pos);
            if (diff == 0) {
                if (enqueue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    slot.data = std::move(item);
                    slot.sequence.store(pos + 1, std::memory_order_release);
                    std::atomic_thread_fence(std::memory_order_seq_cst);
                    if (waiters_empty_.load(std::memory_order_relaxed) != 0) {
                        not_empty_epoch_.fetch_add(1, std::memory_order_release);
                        not_empty_epoch_.notify_one();
                    }
                    return true;
                }
            } else if (diff < 0) {
                return false;
            } else {
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }
    }

    template <typename... Args>
        requires std::constructible_from<T, Args&&...>
    [[nodiscard]] bool try_emplace(Args&&... args) noexcept(
        std::is_nothrow_constructible_v<T, Args&&...>) {
        // Construct `T` on the local stack before entering the CAS loop;
        // move-assignment inside the claimed slot is constrained `noexcept`.
        T value(std::forward<Args>(args)...);
        return try_push(std::move(value));
    }

    [[nodiscard]] bool try_pop(T& out) noexcept(kSlotResetNothrow) {
        const std::size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
        Slot& slot = slots_[pos & kMask];
        const std::size_t seq = slot.sequence.load(std::memory_order_acquire);
        const std::ptrdiff_t diff =
            static_cast<std::ptrdiff_t>(seq) - static_cast<std::ptrdiff_t>(pos + 1);
        if (diff < 0) {
            return false;
        }
        pop_slot_strong(slot.data, out);
        slot.sequence.store(pos + Capacity, std::memory_order_release);
        commit_pop_pos(pos + 1);
        return true;
    }

    [[nodiscard]] std::size_t try_pop_batch(T* out, std::size_t max_count) noexcept(
        kSlotResetNothrow) {
        if (max_count == 0 || out == nullptr) {
            return 0;
        }
        std::size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
        std::size_t popped = 0;
        auto pop_loop = [&]() {
            while (popped < max_count) {
                Slot& slot = slots_[pos & kMask];
                const std::size_t seq = slot.sequence.load(std::memory_order_acquire);
                const std::ptrdiff_t diff =
                    static_cast<std::ptrdiff_t>(seq) - static_cast<std::ptrdiff_t>(pos + 1);
                if (diff < 0) {
                    break;
                }
                pop_slot_strong(slot.data, out[popped]);
                slot.sequence.store(pos + Capacity, std::memory_order_release);
                ++pos;
                ++popped;
            }
        };
        if constexpr (kSlotResetNothrow) {
            pop_loop();
        } else {
            try {
                pop_loop();
            } catch (...) {
                if (popped > 0) {
                    commit_pop_pos(pos);
                }
                throw;
            }
        }
        if (popped > 0) {
            commit_pop_pos(pos);
        }
        return popped;
    }

    [[nodiscard]] std::size_t try_pop_batch(std::span<T> out) noexcept(kSlotResetNothrow) {
        return try_pop_batch(out.data(), out.size());
    }

    // Blocking push with adaptive spin -> yield -> futex (std::atomic::wait) backoff.
    [[nodiscard]] bool push_wait(
        const T& item,
        std::chrono::microseconds max_wait = std::chrono::microseconds::max()) noexcept(
        std::is_nothrow_copy_constructible_v<T>)
        requires std::copy_constructible<T> {
        T staged(item);
        if (try_push(std::move(staged))) {
            return true;
        }
        const bool infinite = (max_wait == std::chrono::microseconds::max());
        const auto deadline =
            infinite ? std::chrono::steady_clock::time_point::max()
                     : (std::chrono::steady_clock::now() + max_wait);

        for (int spin = 0; spin < 32; ++spin) {
            if (try_push(std::move(staged))) {
                return true;
            }
            if (spin >= 16) {
                std::this_thread::yield();
            }
        }

        for (;;) {
            if (!infinite && std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            if (infinite) {
                const std::uint32_t epoch =
                    not_full_epoch_.load(std::memory_order_acquire);
                waiters_full_.fetch_add(1, std::memory_order_seq_cst);
                if (try_push(std::move(staged))) {
                    waiters_full_.fetch_sub(1, std::memory_order_relaxed);
                    return true;
                }
                not_full_epoch_.wait(epoch, std::memory_order_acquire);
                waiters_full_.fetch_sub(1, std::memory_order_relaxed);
            } else {
                if (try_push(std::move(staged))) {
                    return true;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
            if (try_push(std::move(staged))) {
                return true;
            }
        }
    }

    // Blocking pop with adaptive spin -> yield -> futex (std::atomic::wait) backoff.
    [[nodiscard]] bool pop_wait(
        T& out,
        std::chrono::microseconds max_wait = std::chrono::microseconds::max()) noexcept(
        kSlotResetNothrow) {
        if (try_pop(out)) {
            return true;
        }
        const bool infinite = (max_wait == std::chrono::microseconds::max());
        const auto deadline =
            infinite ? std::chrono::steady_clock::time_point::max()
                     : (std::chrono::steady_clock::now() + max_wait);

        for (int spin = 0; spin < 32; ++spin) {
            if (try_pop(out)) {
                return true;
            }
            if (spin >= 16) {
                std::this_thread::yield();
            }
        }

        for (;;) {
            if (!infinite && std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            if (infinite) {
                const std::uint32_t epoch =
                    not_empty_epoch_.load(std::memory_order_acquire);
                waiters_empty_.fetch_add(1, std::memory_order_seq_cst);
                if (try_pop(out)) {
                    waiters_empty_.fetch_sub(1, std::memory_order_relaxed);
                    return true;
                }
                not_empty_epoch_.wait(epoch, std::memory_order_acquire);
                waiters_empty_.fetch_sub(1, std::memory_order_relaxed);
            } else {
                if (try_pop(out)) {
                    return true;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
            if (try_pop(out)) {
                return true;
            }
        }
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
    static void pop_slot_strong(T& slot, T& out) noexcept(kSlotResetNothrow) {
        if constexpr (std::is_trivially_destructible_v<T>) {
            out = std::move(slot);
        } else {
            // Prepare empty replacement before moving from `slot` so a throwing
            // default constructor leaves `slot` and `out` untouched.
            T empty_slot{};
            out = std::move(slot);
            slot = std::move(empty_slot);
        }
    }

    void commit_pop_pos(std::size_t new_pos) noexcept {
        dequeue_pos_.store(new_pos, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (waiters_full_.load(std::memory_order_relaxed) != 0) {
            not_full_epoch_.fetch_add(1, std::memory_order_release);
            not_full_epoch_.notify_all();
        }
    }

    struct alignas(kCacheLineSize) Slot {
        std::atomic<std::size_t> sequence{0};
        T data{};
    };

    alignas(kCacheLineSize) std::atomic<std::size_t> enqueue_pos_{0};
    alignas(kCacheLineSize) std::atomic<std::size_t> dequeue_pos_{0};
    alignas(kCacheLineSize) std::atomic<std::uint32_t> not_empty_epoch_{0};
    std::atomic<std::uint32_t> waiters_empty_{0};
    alignas(kCacheLineSize) std::atomic<std::uint32_t> not_full_epoch_{0};
    std::atomic<std::uint32_t> waiters_full_{0};
    alignas(kCacheLineSize) std::array<Slot, Capacity> slots_{};
};

} // namespace chronoring

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
