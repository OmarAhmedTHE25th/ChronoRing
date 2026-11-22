#include "chronoring/control_plane.hpp"
#include "chronoring/observability.hpp"
#include "chronoring/spsc_ring.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace chronoring::test {

TEST(ObservabilityTest, LineRateTracerAndOtlpExport) {
    LineRateTracer tracer;
    EXPECT_TRUE(tracer.enabled());

    for (std::uint32_t i = 0; i < 500; ++i) {
        ScopedLineRateSpan span(&tracer, TraceStage::TriggerSimd, 256, i, i + 1000U);
    }
    EXPECT_EQ(tracer.recorded_total(), 500U);
    EXPECT_EQ(tracer.dropped_total(), 0U);

    std::vector<TraceSpanEvent> drained(500);
    const std::size_t n = tracer.drain(drained);
    ASSERT_EQ(n, 500U);

    const std::string traces_json =
        export_otlp_traces_json(std::span<const TraceSpanEvent>(drained.data(), 4));
    EXPECT_NE(traces_json.find("\"resourceSpans\""), std::string::npos);
    EXPECT_NE(traces_json.find("\"chronoring.trigger_simd\""), std::string::npos);
    EXPECT_NE(traces_json.find("\"chronoring.batch_size\""), std::string::npos);

    PipelineStats stats{};
    stats.submitted_events = 10000;
    stats.passed_events = 8500;
    stats.rejected_events = 1500;
    stats.p99_ns = 420;
    stats.throughput_meps = 42.5;

    const std::string metrics_json = export_otlp_metrics_json(stats, "chronoring-test");
    EXPECT_NE(metrics_json.find("\"resourceMetrics\""), std::string::npos);
    EXPECT_NE(metrics_json.find("\"chronoring.events.submitted\""), std::string::npos);
    EXPECT_NE(metrics_json.find("\"chronoring-test\""), std::string::npos);

    const std::string prom = export_prometheus_observability(stats, 3, 512, 7);
    EXPECT_NE(prom.find("chronoring_anomalies_detected_total 3"), std::string::npos);
    EXPECT_NE(prom.find("chronoring_autotuner_batch_size 512"), std::string::npos);
    EXPECT_NE(prom.find("chronoring_config_generation 7"), std::string::npos);
}

TEST(ObservabilityTest, LiveConfigReloadZeroDroppedEvents) {
    constexpr std::size_t kTotal = 20'000;
    SpscRingBuffer<EventFrame, 2048> ring;
    LiveConfigStore store;

    std::atomic<bool> producer_done{false};
    std::atomic<bool> stop_reloads{false};
    std::uint64_t consumed = 0;

    // Control plane thread hot-reloading trigger, projection, compression, and routing
    std::thread ctrl([&]() {
        const auto gen1 = store.reload_from_json(
            R"({"min_energy_mev": 5.0, "projection_mask": 31, "enable_compression": true, "routing_mode": "round_robin", "batch_size": 512})");
        EXPECT_TRUE(gen1.has_value());
        std::this_thread::yield();

        LivePipelineConfig next = store.snapshot();
        next.routing_mode = RoutingMode::PredicateFiltered;
        next.projection_mask = kColTimestamps | kColKinematics;
        next.trigger.min_energy_mev = 12.0f;
        (void)store.publish(next);

        while (!stop_reloads.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    });

    std::thread prod([&]() {
        for (std::size_t i = 0; i < kTotal; ++i) {
            EventFrame ev{};
            ev.sequence_id = i;
            ev.energy_mev = 15.0f;
            ev.quality_flags = kFlagValid | kFlagCalibrated;
            ev.multiplicity = 1;
            while (ring.try_push_status(ev) != RingStatus::Ok) {
                std::this_thread::yield();
            }
        }
        producer_done.store(true, std::memory_order_release);
    });

    std::uint64_t last_seen_gen = 1;
    std::vector<EventFrame> batch(128);
    while (consumed < kTotal) {
        const LivePipelineConfig active = store.snapshot();
        EXPECT_GE(active.generation, last_seen_gen);
        last_seen_gen = active.generation;

        const std::size_t popped = ring.try_pop_batch(batch);
        if (popped > 0) {
            consumed += popped;
        } else if (producer_done.load(std::memory_order_acquire) && ring.empty()) {
            break;
        } else {
            std::this_thread::yield();
        }
    }

    stop_reloads.store(true, std::memory_order_release);
    prod.join();
    ctrl.join();

    EXPECT_EQ(consumed, kTotal);
    EXPECT_GE(store.generation(), 3U);
    const LivePipelineConfig final_cfg = store.snapshot();
    EXPECT_TRUE(final_cfg.enable_compression);
    EXPECT_EQ(final_cfg.routing_mode, RoutingMode::PredicateFiltered);
    EXPECT_FLOAT_EQ(final_cfg.trigger.min_energy_mev, 12.0f);
}

TEST(ObservabilityTest, AnomalyDetectionAndAutoTuner) {
    StreamingAnomalyDetector detector(20, 4.0, 0.85);
    for (int i = 0; i < 30; ++i) {
        const double lat = 200.0 + static_cast<double>(i % 5) * 4.0;
        const double rej = 0.05 + static_cast<double>(i % 3) * 0.005;
        EXPECT_EQ(detector.observe(lat, rej, 0.20), 0U);
    }

    std::vector<AnomalyEvent> events;
    const std::size_t hits = detector.observe(5000.0, 0.80, 0.92, &events);
    EXPECT_EQ(hits, 3U);
    ASSERT_EQ(events.size(), 3U);
    EXPECT_EQ(events[0].kind, AnomalyKind::LatencySpike);
    EXPECT_EQ(events[1].kind, AnomalyKind::RejectRateBurst);
    EXPECT_EQ(events[2].kind, AnomalyKind::BackpressureSaturation);
    EXPECT_EQ(detector.total_anomalies(), 3U);

    PipelineAutoTuner tuner(16, 1024, 50'000.0);
    const AutoTunerDecision high_load =
        tuner.recommend(0.82, 85'000.0, 256, -1, NumaPolicy::Default);
    EXPECT_TRUE(high_load.changed);
    EXPECT_EQ(high_load.batch_size, 512U);
    EXPECT_GE(high_load.cpu_core, 0);
    EXPECT_EQ(high_load.numa_policy, NumaPolicy::BindLocalNode);

    const AutoTunerDecision low_fill_high_lat =
        tuner.recommend(0.05, 85'000.0, 256, 1, NumaPolicy::BindLocalNode);
    EXPECT_TRUE(low_fill_high_lat.changed);
    EXPECT_EQ(low_fill_high_lat.batch_size, 128U);
}

} // namespace chronoring::test
