#pragma once

#include "chronoring/event_frame.hpp"
#include "chronoring/mpmc_ring.hpp"
#include "chronoring/numa.hpp"
#include "chronoring/simd_trigger.hpp"
#include "chronoring/status.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace chronoring {

struct LivePipelineConfig {
    TriggerConfig trigger{};
    std::uint16_t projection_mask{kColAll};
    bool enable_compression{false};
    RoutingMode routing_mode{RoutingMode::Broadcast};
    std::size_t batch_size{256};
    int consumer_cpu_core{-1};
    NumaPolicy numa_policy{NumaPolicy::Default};
    std::uint64_t generation{1};
};

// Wait-free lockless live configuration store supporting zero-drop hot reload of
// trigger filters, column projection masks, compression, and routing policies.
class LiveConfigStore {
public:
    static constexpr std::size_t kRingSlots = 16;

    LiveConfigStore() noexcept {
        slots_[0] = LivePipelineConfig{};
    }

    explicit LiveConfigStore(const LivePipelineConfig& initial) noexcept {
        slots_[0] = initial;
        slots_[0].generation = 1;
    }

    // Control-plane update: publishes `cfg` into the next ring slot with release semantics.
    std::uint64_t publish(LivePipelineConfig cfg) noexcept;

    // Parses JSON config updates cleanly onto current snapshot and publishes atomically.
    [[nodiscard]] Expected<std::uint64_t, RingStatus> reload_from_json(
        std::string_view json) noexcept;

    // Hot-path lock-free snapshot with acquire semantics (zero allocations).
    [[nodiscard]] LivePipelineConfig snapshot() const noexcept {
        const std::size_t idx = active_idx_.load(std::memory_order_acquire);
        return slots_[idx];
    }

    [[nodiscard]] std::uint64_t generation() const noexcept {
        return generation_.load(std::memory_order_acquire);
    }

private:
    std::array<LivePipelineConfig, kRingSlots> slots_{};
    std::atomic<std::size_t> active_idx_{0};
    std::atomic<std::uint64_t> generation_{1};
};

enum class AnomalyKind : std::uint8_t {
    LatencySpike           = 0,
    RejectRateBurst        = 1,
    BackpressureSaturation = 2
};

[[nodiscard]] constexpr std::string_view anomaly_kind_name(AnomalyKind kind) noexcept {
    switch (kind) {
    case AnomalyKind::LatencySpike:
        return "LatencySpike";
    case AnomalyKind::RejectRateBurst:
        return "RejectRateBurst";
    case AnomalyKind::BackpressureSaturation:
        return "BackpressureSaturation";
    default:
        return "Unknown";
    }
}

struct AnomalyEvent {
    AnomalyKind kind{AnomalyKind::LatencySpike};
    double observed{0.0};
    double baseline_mean{0.0};
    double z_score{0.0};
    std::uint64_t sample_index{0};
};

// Online Welford mean/stddev streaming anomaly detector for latency, reject rate,
// and ingestion back-pressure saturation.
class StreamingAnomalyDetector {
public:
    explicit StreamingAnomalyDetector(std::size_t warmup_samples = 16,
                                      double z_threshold = 4.0,
                                      double backpressure_limit = 0.85) noexcept
        : warmup_samples_(warmup_samples),
          z_threshold_(z_threshold),
          backpressure_limit_(backpressure_limit) {}

    std::size_t observe(double latency_ns,
                        double reject_rate,
                        double ring_fill_ratio,
                        std::vector<AnomalyEvent>* out_events = nullptr);

    [[nodiscard]] std::uint64_t sample_count() const noexcept { return count_; }
    [[nodiscard]] std::uint64_t total_anomalies() const noexcept { return total_anomalies_; }
    [[nodiscard]] double latency_mean() const noexcept { return lat_mean_; }
    [[nodiscard]] double latency_stddev() const noexcept;
    [[nodiscard]] double reject_rate_mean() const noexcept { return rej_mean_; }

private:
    struct RunningStats {
        void update(double x, std::uint64_t n) noexcept;
        [[nodiscard]] double stddev(std::uint64_t n) const noexcept;
        double mean{0.0};
        double m2{0.0};
    };

    std::size_t warmup_samples_{16};
    double z_threshold_{4.0};
    double backpressure_limit_{0.85};
    std::uint64_t count_{0};
    std::uint64_t total_anomalies_{0};
    double lat_mean_{0.0};
    double rej_mean_{0.0};
    RunningStats lat_stats_{};
    RunningStats rej_stats_{};
};

struct AutoTunerDecision {
    std::size_t batch_size{256};
    int cpu_core{-1};
    NumaPolicy numa_policy{NumaPolicy::Default};
    bool changed{false};
};

// Closed-loop auto-tuner adapting batch size, core affinity, and NUMA placement
// from live ring fill ratio and p99 tail latency.
class PipelineAutoTuner {
public:
    PipelineAutoTuner(std::size_t min_batch = 16,
                      std::size_t max_batch = 1024,
                      double target_p99_ns = 50'000.0) noexcept
        : min_batch_(min_batch), max_batch_(max_batch), target_p99_ns_(target_p99_ns) {}

    [[nodiscard]] AutoTunerDecision recommend(double ring_fill_ratio,
                                              double p99_ns,
                                              std::size_t current_batch_size,
                                              int current_core,
                                              NumaPolicy current_numa) const noexcept;

private:
    std::size_t min_batch_{16};
    std::size_t max_batch_{1024};
    double target_p99_ns_{50'000.0};
};

} // namespace chronoring
