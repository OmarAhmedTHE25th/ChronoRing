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
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>

namespace chronoring {

// Wait-free bounded Single-Producer Single-Consumer ring buffer.
//
// Memory-ordering contract:
//  - `prod_.head` is written only by the producer (`memory_order_release`) so that all
//    preceding writes into `buffer_[head & kMask]` become visible before the updated head.
//  - `prod_.cached_tail` is producer-private and only refreshed via an acquire load of
//    `cons_.tail` when the local bound appears full, eliminating cross-core cache bouncing.
//  - Symmetric acquire/release pairs apply to `cons_.tail` and `cons_.cached_head`.
template <typename T, std::size_t Capacity>
    requires (Capacity >= 2 && (Capacity & (Capacity - 1)) == 0) &&
             std::default_initializable<T> &&
             std::movable<T> &&
             std::is_nothrow_destructible_v<T>
class SpscRingBuffer {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                  "SpscRingBuffer Capacity must be a power of 2 and >= 2");
    static_assert(std::is_nothrow_destructible_v<T>,
                  "T must have noexcept destructor");

public:
    static constexpr std::size_t kCapacity = Capacity;
    static constexpr std::size_t kMask = Capacity - 1;

    SpscRingBuffer() = default;
    SpscRingBuffer(const SpscRingBuffer&) = delete;
    SpscRingBuffer& operator=(const SpscRingBuffer&) = delete;
    SpscRingBuffer(SpscRingBuffer&&) = delete;
    SpscRingBuffer& operator=(SpscRingBuffer&&) = delete;

    [[nodiscard]] bool can_push() noexcept {
        const std::size_t head = prod_.head.load(std::memory_order_relaxed);
        if ((head - prod_.cached_tail) < Capacity) {
            return true;
        }
        prod_.cached_tail = cons_.tail.load(std::memory_order_acquire);
        return (head - prod_.cached_tail) < Capacity;
    }

    [[nodiscard]] RingStatus try_push_status(const T& item) noexcept(
        std::is_nothrow_copy_assignable_v<T>)
        requires std::is_copy_assignable_v<T> {
        return try_push(item) ? RingStatus::Ok : RingStatus::Full;
    }

    [[nodiscard]] RingStatus try_pop_status(T& out) noexcept(
        std::is_nothrow_move_assignable_v<T>) {
        return try_pop(out) ? RingStatus::Ok : RingStatus::Empty;
    }

    [[nodiscard]] bool try_push(const T& item) noexcept(std::is_nothrow_copy_assignable_v<T>)
        requires std::is_copy_assignable_v<T> {
#ifndef NDEBUG
        assert(!prod_.has_active_write_reservation &&
               "try_push() called while an active write reservation is held");
#endif
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
#ifndef NDEBUG
        assert(!prod_.has_active_write_reservation &&
               "try_push() called while an active write reservation is held");
#endif
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
#ifndef NDEBUG
        assert(!prod_.has_active_write_reservation &&
               "try_emplace() called while an active write reservation is held");
#endif
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

    // Zero-copy producer reservation: call `commit_push()` after writing `*ptr`.
    [[nodiscard]] T* try_reserve_push() noexcept {
#ifndef NDEBUG
        assert(!prod_.has_active_write_reservation &&
               "try_reserve_push() called while a write reservation is already active");
#endif
        const std::size_t head = prod_.head.load(std::memory_order_relaxed);
        if ((head - prod_.cached_tail) >= Capacity) {
            prod_.cached_tail = cons_.tail.load(std::memory_order_acquire);
            if ((head - prod_.cached_tail) >= Capacity) {
                return nullptr;
            }
        }
#ifndef NDEBUG
        prod_.has_active_write_reservation = true;
#endif
        return &buffer_[head & kMask];
    }

    void commit_push() noexcept {
#ifndef NDEBUG
        assert(prod_.has_active_write_reservation &&
               "commit_push() called without an active write reservation");
        prod_.has_active_write_reservation = false;
#endif
        const std::size_t head = prod_.head.load(std::memory_order_relaxed);
        prod_.head.store(head + 1, std::memory_order_release);
    }

    // Zero-copy consumer peek: call `consume_pop()` when finished reading `*ptr`.
    [[nodiscard]] const T* try_peek() noexcept {
#ifndef NDEBUG
        assert(!cons_.has_active_read_reservation &&
               "try_peek() called while a read reservation is already active");
#endif
        const std::size_t tail = cons_.tail.load(std::memory_order_relaxed);
        if (cons_.cached_head == tail) {
            cons_.cached_head = prod_.head.load(std::memory_order_acquire);
            if (cons_.cached_head == tail) {
                return nullptr;
            }
        }
#ifndef NDEBUG
        cons_.has_active_read_reservation = true;
#endif
        return &buffer_[tail & kMask];
    }

    void consume_pop() noexcept {
#ifndef NDEBUG
        assert(cons_.has_active_read_reservation &&
               "consume_pop() called without an active read reservation");
        cons_.has_active_read_reservation = false;
#endif
        const std::size_t tail = cons_.tail.load(std::memory_order_relaxed);
        if constexpr (!std::is_trivially_destructible_v<T>) {
            buffer_[tail & kMask] = T{};
        }
        cons_.tail.store(tail + 1, std::memory_order_release);
    }

    // Move-only RAII guard for zero-copy SPSC producer/consumer reservations.
    // Automatically commits on destruction unless already committed via `.commit()`.
    template <bool IsWrite>
    class BasicReservation {
    public:
        using Pointer = std::conditional_t<IsWrite, T*, const T*>;
        using Reference = std::conditional_t<IsWrite, T&, const T&>;

        BasicReservation() noexcept = default;
        BasicReservation(const BasicReservation&) = delete;
        BasicReservation& operator=(const BasicReservation&) = delete;

        BasicReservation(BasicReservation&& other) noexcept
            : ring_(std::exchange(other.ring_, nullptr)),
              slot_(std::exchange(other.slot_, nullptr)) {}

        BasicReservation& operator=(BasicReservation&& other) noexcept {
            if (this != &other) {
                commit();
                ring_ = std::exchange(other.ring_, nullptr);
                slot_ = std::exchange(other.slot_, nullptr);
            }
            return *this;
        }

        ~BasicReservation() {
            commit();
        }

        void commit() noexcept {
            if (ring_ != nullptr) {
                if constexpr (IsWrite) {
                    ring_->commit_push();
                } else {
                    ring_->consume_pop();
                }
                ring_ = nullptr;
                slot_ = nullptr;
            }
        }

        [[nodiscard]] bool valid() const noexcept { return slot_ != nullptr; }
        [[nodiscard]] explicit operator bool() const noexcept { return valid(); }
        [[nodiscard]] Pointer get() const noexcept { return slot_; }
        [[nodiscard]] Reference operator*() const noexcept { return *slot_; }
        [[nodiscard]] Pointer operator->() const noexcept { return slot_; }

    private:
        friend class SpscRingBuffer;
        BasicReservation(SpscRingBuffer* ring, Pointer slot) noexcept
            : ring_(ring), slot_(slot) {}

        SpscRingBuffer* ring_{nullptr};
        Pointer slot_{nullptr};
    };

    using WriteReservation = BasicReservation<true>;
    using ReadReservation = BasicReservation<false>;

    [[nodiscard]] WriteReservation reserve_push() noexcept {
        T* ptr = try_reserve_push();
        return ptr != nullptr ? WriteReservation(this, ptr) : WriteReservation();
    }

    [[nodiscard]] ReadReservation reserve_pop() noexcept {
        const T* ptr = try_peek();
        return ptr != nullptr ? ReadReservation(this, ptr) : ReadReservation();
    }

    [[nodiscard]] bool try_pop(T& out) noexcept(std::is_nothrow_move_assignable_v<T>) {
#ifndef NDEBUG
        assert(!cons_.has_active_read_reservation &&
               "try_pop() called while an active read reservation is held");
#endif
        const std::size_t tail = cons_.tail.load(std::memory_order_relaxed);
        if (cons_.cached_head == tail) {
            cons_.cached_head = prod_.head.load(std::memory_order_acquire);
            if (cons_.cached_head == tail) {
                return false;
            }
        }
        const std::size_t slot_idx = tail & kMask;
        out = std::move(buffer_[slot_idx]);
        if constexpr (!std::is_trivially_destructible_v<T>) {
            buffer_[slot_idx] = T{};
        }
        cons_.tail.store(tail + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] std::size_t try_push_batch(const T* items, std::size_t count) noexcept(
        std::is_nothrow_copy_assignable_v<T>)
        requires std::is_copy_assignable_v<T> {
#ifndef NDEBUG
        assert(!prod_.has_active_write_reservation &&
               "try_push_batch() called while an active write reservation is held");
#endif
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

    [[nodiscard]] std::size_t try_push_batch(std::span<const T> items) noexcept(
        std::is_nothrow_copy_assignable_v<T>)
        requires std::is_copy_assignable_v<T> {
        return try_push_batch(items.data(), items.size());
    }

    [[nodiscard]] std::size_t try_pop_batch(T* out, std::size_t max_count) noexcept(
        std::is_nothrow_move_assignable_v<T>) {
#ifndef NDEBUG
        assert(!cons_.has_active_read_reservation &&
               "try_pop_batch() called while an active read reservation is held");
#endif
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
            const std::size_t slot_idx = (tail + i) & kMask;
            out[i] = std::move(buffer_[slot_idx]);
            if constexpr (!std::is_trivially_destructible_v<T>) {
                buffer_[slot_idx] = T{};
            }
        }
        cons_.tail.store(tail + to_pop, std::memory_order_release);
        return to_pop;
    }

    [[nodiscard]] std::size_t try_pop_batch(std::span<T> out) noexcept(
        std::is_nothrow_move_assignable_v<T>) {
        return try_pop_batch(out.data(), out.size());
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
#ifndef NDEBUG
        bool has_active_write_reservation{false};
#endif
    } prod_;

    struct alignas(kCacheLineSize) ConsumerState {
        std::atomic<std::size_t> tail{0};
        std::size_t cached_head{0};
#ifndef NDEBUG
        bool has_active_read_reservation{false};
#endif
    } cons_;

    alignas(kCacheLineSize) std::array<T, Capacity> buffer_{};
};

// Multi-consumer wait-free broadcast fan-out built atop NumConsumers isolated SPSC channels.
template <typename T, std::size_t Capacity, std::size_t NumConsumers>
    requires (NumConsumers >= 1) &&
             (Capacity >= 2 && (Capacity & (Capacity - 1)) == 0) &&
             std::default_initializable<T> &&
             std::movable<T> &&
             std::is_nothrow_destructible_v<T>
class SpscFanOut {
    static_assert(NumConsumers >= 1, "SpscFanOut requires at least 1 consumer");

public:
    static constexpr std::size_t kNumConsumers = NumConsumers;

    SpscFanOut() = default;
    SpscFanOut(const SpscFanOut&) = delete;
    SpscFanOut& operator=(const SpscFanOut&) = delete;

    // Best-effort broadcast to every consumer channel; returns number of channels pushed.
    [[nodiscard]] std::size_t broadcast(const T& item) noexcept(
        std::is_nothrow_copy_assignable_v<T>)
        requires std::is_copy_assignable_v<T> {
        std::size_t delivered = 0;
        for (std::size_t i = 0; i < NumConsumers; ++i) {
            if (channels_[i].try_push(item)) {
                ++delivered;
            }
        }
        return delivered;
    }

    // All-or-nothing atomic-check broadcast from the single producer thread.
    [[nodiscard]] bool broadcast_all(const T& item) noexcept(
        std::is_nothrow_copy_assignable_v<T>)
        requires std::is_copy_assignable_v<T> {
        for (std::size_t i = 0; i < NumConsumers; ++i) {
            if (!channels_[i].can_push()) {
                return false;
            }
        }
        for (std::size_t i = 0; i < NumConsumers; ++i) {
            (void)channels_[i].try_push(item);
        }
        return true;
    }

    [[nodiscard]] bool try_pop(std::size_t consumer_idx, T& out) noexcept(
        std::is_nothrow_move_assignable_v<T>) {
        if (consumer_idx >= NumConsumers) {
            return false;
        }
        return channels_[consumer_idx].try_pop(out);
    }

    [[nodiscard]] std::size_t try_pop_batch(std::size_t consumer_idx, std::span<T> out) noexcept(
        std::is_nothrow_move_assignable_v<T>) {
        if (consumer_idx >= NumConsumers) {
            return 0;
        }
        return channels_[consumer_idx].try_pop_batch(out);
    }

    [[nodiscard]] std::size_t size_approx(std::size_t consumer_idx) const noexcept {
        if (consumer_idx >= NumConsumers) {
            return 0;
        }
        return channels_[consumer_idx].size_approx();
    }

    [[nodiscard]] static constexpr std::size_t num_consumers() noexcept {
        return NumConsumers;
    }

private:
    std::array<SpscRingBuffer<T, Capacity>, NumConsumers> channels_{};
};

} // namespace chronoring

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
