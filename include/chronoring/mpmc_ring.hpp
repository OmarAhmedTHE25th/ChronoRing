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
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>

namespace chronoring {

// Lock-free bounded Multi-Producer Multi-Consumer ring buffer (Vyukov MPMC protocol).
//
// Memory-ordering contract:
//   - Producers contend cleanly on `enqueue_pos_` using `memory_order_relaxed` CAS, claiming
//     ownership of `slots_[pos & kMask]` when `sequence.load(acquire) == pos`.
//   - After constructing `T` in-place, the winning producer executes `sequence.store(pos + 1, release)`.
//   - Consumers contend cleanly on `dequeue_pos_` using `memory_order_relaxed` CAS, claiming
//     slot ownership when `sequence.load(acquire) == pos + 1`, then advancing `sequence` to
//     `pos + Capacity` with `memory_order_release`.
template <typename T, std::size_t Capacity>
class MpmcRingBuffer {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                  "MpmcRingBuffer Capacity must be a power of 2 and >= 2");
    static_assert(std::is_move_constructible_v<T> || std::is_copy_constructible_v<T>,
                  "T must be copy or move constructible");

    static constexpr std::size_t kMask = Capacity - 1;

public:
    MpmcRingBuffer() noexcept {
        for (std::size_t i = 0; i < Capacity; ++i) {
            slots_[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    MpmcRingBuffer(const MpmcRingBuffer&) = delete;
    MpmcRingBuffer& operator=(const MpmcRingBuffer&) = delete;
    MpmcRingBuffer(MpmcRingBuffer&&) = delete;
    MpmcRingBuffer& operator=(MpmcRingBuffer&&) = delete;

    template <typename... Args>
    [[nodiscard]] RingStatus try_emplace_status(Args&&... args) noexcept(
        std::is_nothrow_constructible_v<T, Args...>) {
        Slot* slot = nullptr;
        std::size_t pos = enqueue_pos_.load(std::memory_order_relaxed);

        for (;;) {
            slot = &slots_[pos & kMask];
            const std::size_t seq = slot->sequence.load(std::memory_order_acquire);
            const std::ptrdiff_t diff =
                static_cast<std::ptrdiff_t>(seq) - static_cast<std::ptrdiff_t>(pos);
            if (diff == 0) {
                if (enqueue_pos_.compare_exchange_weak(
                        pos, pos + 1, std::memory_order_relaxed, std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return RingStatus::Full;
            } else {
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }

        slot->data = T(std::forward<Args>(args)...);
        slot->sequence.store(pos + 1, std::memory_order_release);
        return RingStatus::Ok;
    }

    [[nodiscard]] RingStatus try_push_status(const T& item) noexcept(
        std::is_nothrow_copy_assignable_v<T>) {
        return try_emplace_status(item);
    }

    [[nodiscard]] RingStatus try_push_status(T&& item) noexcept(
        std::is_nothrow_move_assignable_v<T>) {
        return try_emplace_status(std::move(item));
    }

    [[nodiscard]] bool try_push(const T& item) noexcept(
        std::is_nothrow_copy_assignable_v<T>) {
        return try_push_status(item) == RingStatus::Ok;
    }

    [[nodiscard]] bool try_push(T&& item) noexcept(
        std::is_nothrow_move_assignable_v<T>) {
        return try_push_status(std::move(item)) == RingStatus::Ok;
    }

    [[nodiscard]] RingStatus try_pop_status(T& out) noexcept(
        std::is_nothrow_move_assignable_v<T>) {
        Slot* slot = nullptr;
        std::size_t pos = dequeue_pos_.load(std::memory_order_relaxed);

        for (;;) {
            slot = &slots_[pos & kMask];
            const std::size_t seq = slot->sequence.load(std::memory_order_acquire);
            const std::ptrdiff_t diff =
                static_cast<std::ptrdiff_t>(seq) - static_cast<std::ptrdiff_t>(pos + 1);
            if (diff == 0) {
                if (dequeue_pos_.compare_exchange_weak(
                        pos, pos + 1, std::memory_order_relaxed, std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return RingStatus::Empty;
            } else {
                pos = dequeue_pos_.load(std::memory_order_relaxed);
            }
        }

        out = std::move(slot->data);
        slot->sequence.store(pos + Capacity, std::memory_order_release);
        return RingStatus::Ok;
    }

    [[nodiscard]] bool try_pop(T& out) noexcept(
        std::is_nothrow_move_assignable_v<T>) {
        return try_pop_status(out) == RingStatus::Ok;
    }

    [[nodiscard]] std::size_t try_push_batch(const T* items, std::size_t count) noexcept {
        if (items == nullptr || count == 0) {
            return 0;
        }
        std::size_t pushed = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (!try_push(items[i])) {
                break;
            }
            ++pushed;
        }
        return pushed;
    }

    [[nodiscard]] std::size_t try_push_batch(std::span<const T> items) noexcept {
        return try_push_batch(items.data(), items.size());
    }

    [[nodiscard]] std::size_t try_pop_batch(T* out, std::size_t max_count) noexcept {
        if (out == nullptr || max_count == 0) {
            return 0;
        }
        std::size_t popped = 0;
        while (popped < max_count && try_pop(out[popped])) {
            ++popped;
        }
        return popped;
    }

    [[nodiscard]] std::size_t try_pop_batch(std::span<T> out) noexcept {
        return try_pop_batch(out.data(), out.size());
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

    [[nodiscard]] bool empty() const noexcept { return size_approx() == 0; }
    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
    struct alignas(kCacheLineSize) Slot {
        std::atomic<std::size_t> sequence{0};
        T data{};
    };

    alignas(kCacheLineSize) std::atomic<std::size_t> enqueue_pos_{0};
    alignas(kCacheLineSize) std::atomic<std::size_t> dequeue_pos_{0};
    alignas(kCacheLineSize) std::array<Slot, Capacity> slots_{};
};

enum class RoutingMode : std::uint8_t {
    Broadcast         = 0,
    RoundRobin        = 1,
    PredicateFiltered = 2
};

// Arbitrary M-producer to N-consumer-group lock-free topology router.
// Any producer thread can push concurrently via Broadcast, RoundRobin, or PredicateFiltered.
template <typename T, std::size_t Capacity, std::size_t NumOutputs>
class RingTopologyRouter {
    static_assert(NumOutputs >= 1, "RingTopologyRouter requires at least 1 output ring");

public:
    using FilterPredicate = bool (*)(const T& item) noexcept;

    void set_mode(RoutingMode mode) noexcept { mode_ = mode; }
    [[nodiscard]] RoutingMode mode() const noexcept { return mode_; }

    void set_output_predicate(std::size_t out_idx, FilterPredicate pred) noexcept {
        if (out_idx < NumOutputs) {
            predicates_[out_idx] = pred;
        }
    }

    // Returns bitmask of output indices that accepted the pushed item.
    [[nodiscard]] std::uint64_t route_push(const T& item) noexcept {
        std::uint64_t delivered_mask = 0;
        switch (mode_) {
        case RoutingMode::Broadcast:
            for (std::size_t i = 0; i < NumOutputs; ++i) {
                if (channels_[i].try_push(item)) {
                    if (i < 64U) {
                        delivered_mask |= (std::uint64_t{1} << i);
                    }
                }
            }
            break;
        case RoutingMode::RoundRobin: {
            const std::size_t idx =
                rr_cursor_.fetch_add(1, std::memory_order_relaxed) % NumOutputs;
            if (channels_[idx].try_push(item) && idx < 64U) {
                delivered_mask |= (std::uint64_t{1} << idx);
            }
            break;
        }
        case RoutingMode::PredicateFiltered:
            for (std::size_t i = 0; i < NumOutputs; ++i) {
                if (predicates_[i] == nullptr || predicates_[i](item)) {
                    if (channels_[i].try_push(item) && i < 64U) {
                        delivered_mask |= (std::uint64_t{1} << i);
                    }
                }
            }
            break;
        }
        return delivered_mask;
    }

    [[nodiscard]] RingStatus try_pop_status(std::size_t out_idx, T& out) noexcept {
        if (out_idx >= NumOutputs) {
            return RingStatus::InvalidArgument;
        }
        return channels_[out_idx].try_pop_status(out);
    }

    [[nodiscard]] bool try_pop(std::size_t out_idx, T& out) noexcept {
        return try_pop_status(out_idx, out) == RingStatus::Ok;
    }

    [[nodiscard]] MpmcRingBuffer<T, Capacity>& channel(std::size_t out_idx) noexcept {
        return channels_[out_idx];
    }

    [[nodiscard]] std::size_t size_approx(std::size_t out_idx) const noexcept {
        if (out_idx >= NumOutputs) {
            return 0;
        }
        return channels_[out_idx].size_approx();
    }

    [[nodiscard]] static constexpr std::size_t num_outputs() noexcept {
        return NumOutputs;
    }

private:
    alignas(kCacheLineSize) std::atomic<std::size_t> rr_cursor_{0};
    RoutingMode mode_{RoutingMode::Broadcast};
    std::array<FilterPredicate, NumOutputs> predicates_{};
    std::array<MpmcRingBuffer<T, Capacity>, NumOutputs> channels_{};
};

} // namespace chronoring

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
