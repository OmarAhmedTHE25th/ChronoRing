#pragma once

#include "chronoring/pipeline.hpp"
#include "chronoring/spsc_ring.hpp"
#include "chronoring/status.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace chronoring {

enum class TraceStage : std::uint16_t {
    Ingest        = 0,
    TriggerSimd   = 1,
    CompactSoa    = 2,
    ColumnarWrite = 3,
    GroupCommit   = 4,
    ConfigReload  = 5
};

[[nodiscard]] constexpr std::string_view trace_stage_name(TraceStage stage) noexcept {
    switch (stage) {
    case TraceStage::Ingest:
        return "chronoring.ingest";
    case TraceStage::TriggerSimd:
        return "chronoring.trigger_simd";
    case TraceStage::CompactSoa:
        return "chronoring.compact_soa";
    case TraceStage::ColumnarWrite:
        return "chronoring.columnar_write";
    case TraceStage::GroupCommit:
        return "chronoring.group_commit";
    case TraceStage::ConfigReload:
        return "chronoring.config_reload";
    default:
        return "chronoring.unknown";
    }
}

struct alignas(32) TraceSpanEvent {
    std::uint64_t start_ns{0};
    std::uint64_t duration_ns{0};
    std::uint32_t trace_id{0};
    std::uint32_t span_id{0};
    std::uint32_t batch_size{0};
    std::uint16_t stage{0};
    std::uint16_t flags{0};
};

static_assert(sizeof(TraceSpanEvent) == 32, "TraceSpanEvent must be exactly 32 bytes");
static_assert(std::is_trivially_copyable_v<TraceSpanEvent>);

// Line-rate wait-free SPSC trace ring with zero heap allocation on hot paths.
class LineRateTracer {
public:
    static constexpr std::size_t kCapacity = 4096;

    LineRateTracer() noexcept = default;

    void set_enabled(bool enabled) noexcept {
        enabled_.store(enabled, std::memory_order_release);
    }
    [[nodiscard]] bool enabled() const noexcept {
        return enabled_.load(std::memory_order_acquire);
    }

    void record(TraceStage stage,
                std::uint64_t start_ns,
                std::uint64_t duration_ns,
                std::uint32_t batch_size,
                std::uint32_t trace_id = 0,
                std::uint32_t span_id = 0,
                std::uint16_t flags = 0) noexcept {
        if (!enabled_.load(std::memory_order_relaxed)) {
            return;
        }
        TraceSpanEvent ev{};
        ev.start_ns = start_ns;
        ev.duration_ns = duration_ns;
        ev.trace_id = trace_id;
        ev.span_id = span_id;
        ev.batch_size = batch_size;
        ev.stage = static_cast<std::uint16_t>(stage);
        ev.flags = flags;
        if (ring_.try_push_status(ev) == RingStatus::Ok) {
            recorded_.fetch_add(1, std::memory_order_relaxed);
        } else {
            dropped_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] std::size_t drain(std::span<TraceSpanEvent> out) noexcept {
        return ring_.try_pop_batch(out);
    }

    [[nodiscard]] std::uint64_t recorded_total() const noexcept {
        return recorded_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t dropped_total() const noexcept {
        return dropped_.load(std::memory_order_relaxed);
    }

private:
    SpscRingBuffer<TraceSpanEvent, kCapacity> ring_{};
    std::atomic<bool> enabled_{true};
    std::atomic<std::uint64_t> recorded_{0};
    std::atomic<std::uint64_t> dropped_{0};
};

class ScopedLineRateSpan {
public:
    ScopedLineRateSpan(LineRateTracer* tracer,
                       TraceStage stage,
                       std::uint32_t batch_size = 0,
                       std::uint32_t trace_id = 0,
                       std::uint32_t span_id = 0) noexcept
        : tracer_(tracer),
          stage_(stage),
          batch_size_(batch_size),
          trace_id_(trace_id),
          span_id_(span_id),
          start_ns_(tracer_ != nullptr ? monotonic_now_ns() : 0ULL) {}

    ~ScopedLineRateSpan() {
        if (tracer_ != nullptr) {
            const std::uint64_t end_ns = monotonic_now_ns();
            const std::uint64_t dur = (end_ns >= start_ns_) ? (end_ns - start_ns_) : 0ULL;
            tracer_->record(stage_, start_ns_, dur, batch_size_, trace_id_, span_id_);
        }
    }

    ScopedLineRateSpan(const ScopedLineRateSpan&) = delete;
    ScopedLineRateSpan& operator=(const ScopedLineRateSpan&) = delete;

private:
    static std::uint64_t monotonic_now_ns() noexcept {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }

    LineRateTracer* tracer_;
    TraceStage stage_;
    std::uint32_t batch_size_;
    std::uint32_t trace_id_;
    std::uint32_t span_id_;
    std::uint64_t start_ns_;
};

[[nodiscard]] std::string export_otlp_metrics_json(
    const PipelineStats& stats,
    std::string_view service_name = "chronoring");

[[nodiscard]] std::string export_otlp_traces_json(
    std::span<const TraceSpanEvent> spans,
    std::string_view service_name = "chronoring");

[[nodiscard]] std::string export_prometheus_observability(
    const PipelineStats& stats,
    std::uint64_t anomalies_detected,
    std::size_t tuned_batch_size,
    std::uint64_t config_generation);

} // namespace chronoring
