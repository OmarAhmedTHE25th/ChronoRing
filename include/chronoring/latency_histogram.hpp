#pragma once

#include "chronoring/event_frame.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace chronoring {

class LatencyHistogram {
public:
    static constexpr std::size_t kSubBucketBits = 7;
    static constexpr std::size_t kSubBucketCount = std::size_t{1} << kSubBucketBits; // 128 (< 0.8% bucket width)
    static constexpr std::size_t kNumGroups = 32; // Covers 0 ns up to ~274 seconds
    static constexpr std::size_t kTotalBuckets = kNumGroups * kSubBucketCount; // 4096

    LatencyHistogram() noexcept = default;
    LatencyHistogram(const LatencyHistogram&) = delete;
    LatencyHistogram& operator=(const LatencyHistogram&) = delete;

    void record(std::uint64_t latency_ns) noexcept {
        const std::size_t idx = bucket_index(latency_ns);
        buckets_[idx].fetch_add(1, std::memory_order_relaxed);
        count_.fetch_add(1, std::memory_order_relaxed);
        sum_ns_.fetch_add(latency_ns, std::memory_order_relaxed);

        std::uint64_t prev_min = min_ns_.load(std::memory_order_relaxed);
        while (latency_ns < prev_min &&
               !min_ns_.compare_exchange_weak(prev_min, latency_ns,
                                              std::memory_order_relaxed,
                                              std::memory_order_relaxed)) {
        }

        std::uint64_t prev_max = max_ns_.load(std::memory_order_relaxed);
        while (latency_ns > prev_max &&
               !max_ns_.compare_exchange_weak(prev_max, latency_ns,
                                              std::memory_order_relaxed,
                                              std::memory_order_relaxed)) {
        }
    }

    void reset() noexcept {
        for (auto& b : buckets_) {
            b.store(0, std::memory_order_relaxed);
        }
        count_.store(0, std::memory_order_relaxed);
        sum_ns_.store(0, std::memory_order_relaxed);
        min_ns_.store(std::numeric_limits<std::uint64_t>::max(), std::memory_order_relaxed);
        max_ns_.store(0, std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t count() const noexcept {
        return count_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t min_ns() const noexcept {
        if (count() == 0) {
            return 0;
        }
        return min_ns_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t max_ns() const noexcept {
        if (count() == 0) {
            return 0;
        }
        return max_ns_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] double mean_ns() const noexcept {
        const std::uint64_t n = count();
        if (n == 0) {
            return 0.0;
        }
        return static_cast<double>(sum_ns_.load(std::memory_order_relaxed)) /
               static_cast<double>(n);
    }

    [[nodiscard]] std::uint64_t percentile(double p) const noexcept {
        const std::uint64_t total = count();
        if (total == 0) {
            return 0;
        }
        const double clamped_p = std::clamp(p, 0.0, 100.0);
        const std::uint64_t min_val = min_ns();
        const std::uint64_t max_val = max_ns();
        if (clamped_p <= 0.0) {
            return min_val;
        }
        const std::uint64_t target = std::clamp<std::uint64_t>(
            static_cast<std::uint64_t>(std::ceil((clamped_p / 100.0) * static_cast<double>(total))),
            1, total);

        std::uint64_t running = 0;
        for (std::size_t i = 0; i < kTotalBuckets; ++i) {
            running += buckets_[i].load(std::memory_order_relaxed);
            if (running >= target) {
                return std::clamp(bucket_midpoint_ns(i), min_val, max_val);
            }
        }
        return max_val;
    }

    [[nodiscard]] std::uint64_t p50() const noexcept { return percentile(50.0); }
    [[nodiscard]] std::uint64_t p90() const noexcept { return percentile(90.0); }
    [[nodiscard]] std::uint64_t p99() const noexcept { return percentile(99.0); }
    [[nodiscard]] std::uint64_t p99_9() const noexcept { return percentile(99.9); }

    [[nodiscard]] static std::size_t bucket_index(std::uint64_t value_ns) noexcept {
        if (value_ns < kSubBucketCount) {
            return static_cast<std::size_t>(value_ns);
        }
        const unsigned msb = static_cast<unsigned>(63 - std::countl_zero(value_ns));
        const std::size_t group = static_cast<std::size_t>(msb) - (kSubBucketBits - 1);
        if (group >= kNumGroups) {
            return kTotalBuckets - 1;
        }
        const unsigned shift = static_cast<unsigned>(group - 1);
        const std::size_t sub =
            static_cast<std::size_t>((value_ns >> shift) & (kSubBucketCount - 1));
        return group * kSubBucketCount + sub;
    }

    [[nodiscard]] static std::uint64_t bucket_midpoint_ns(std::size_t idx) noexcept {
        if (idx < kSubBucketCount) {
            return static_cast<std::uint64_t>(idx);
        }
        const std::size_t group = idx / kSubBucketCount;
        const std::size_t sub = idx % kSubBucketCount;
        const unsigned shift = static_cast<unsigned>(group - 1);
        const std::uint64_t low =
            (static_cast<std::uint64_t>(kSubBucketCount) + sub) << shift;
        const std::uint64_t half_step =
            (shift == 0) ? 0ULL : (std::uint64_t{1} << (shift - 1));
        return low + half_step;
    }

private:
    alignas(kCacheLineSize) std::array<std::atomic<std::uint64_t>, kTotalBuckets> buckets_{};
    alignas(kCacheLineSize) std::atomic<std::uint64_t> count_{0};
    std::atomic<std::uint64_t> sum_ns_{0};
    std::atomic<std::uint64_t> min_ns_{std::numeric_limits<std::uint64_t>::max()};
    std::atomic<std::uint64_t> max_ns_{0};
};

} // namespace chronoring
