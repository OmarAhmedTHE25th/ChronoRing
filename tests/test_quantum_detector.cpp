#include "chronoring/pipeline.hpp"
#include "chronoring/quantum_detector.hpp"
#include "chronoring/simd_trigger.hpp"
#include "chronoring/spsc_ring.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <span>
#include <vector>

namespace chronoring::test {

TEST(QuantumDetectorTest, SnspdTesAndMkidEncodingWithHealthFlags) {
    QuantumCalibration cal{};
    cal.gain_mev_per_unit = 2.0f;
    cal.offset_mev = 0.5f;
    cal.time_walk_ps_per_unit = 0.25f;
    cal.nominal_bath_temp_mk = 100.0f;
    cal.max_temp_drift_mk = 5.0f;

    QuantumDetectorSource src(cal);

    // 1. Healthy SNSPD event
    QuantumEventSample snspd{};
    snspd.timestamp_ns = 1000ULL;
    snspd.sequence_id = 1ULL;
    snspd.pixel_id = 17U;
    snspd.arrival_ps = 50.0f;
    snspd.pulse_integral = 10.0f;
    snspd.bath_temp_mk = 101.0f;
    snspd.coincidence_count = 3;
    snspd.kind = CryoDetectorKind::SNSPD;

    const EventFrame f_snspd = src.encode(snspd);
    EXPECT_FLOAT_EQ(f_snspd.energy_mev, 20.5f);
    EXPECT_FLOAT_EQ(f_snspd.timing_ps, 47.5f);
    EXPECT_EQ(f_snspd.quality_flags, kFlagValid | kFlagCalibrated);
    EXPECT_EQ(f_snspd.multiplicity, 3U);
    EXPECT_EQ(f_snspd.trigger_mask, 1U << static_cast<unsigned>(CryoDetectorKind::SNSPD));

    // 2. MKID complex IQ readout
    QuantumEventSample mkid{};
    mkid.iq_i = 3.0f;
    mkid.iq_q = 4.0f;
    mkid.kind = CryoDetectorKind::MKID;
    const EventFrame f_mkid = src.encode(mkid);
    EXPECT_FLOAT_EQ(f_mkid.energy_mev, 10.5f); // hypot(3,4)=5 * 2.0 + 0.5
    EXPECT_NEAR(f_mkid.pos_z_mm, std::atan2(4.0f, 3.0f), 1e-5f);

    // 3. Quench + thermal drift propagation
    QuantumEventSample quenched{};
    quenched.kind = CryoDetectorKind::TES;
    quenched.pulse_integral = 5.0f;
    quenched.bath_temp_mk = 112.0f; // > nominal + 5 mK
    quenched.health_flags = kCryoQuenchDetected;
    const EventFrame f_quench = src.encode(quenched);
    EXPECT_NE((f_quench.quality_flags & kFlagVeto), 0u);
    EXPECT_NE((f_quench.quality_flags & kFlagPileupWarn), 0u);
}

TEST(QuantumDetectorTest, PresetsAndRingPipelineIngestion) {
    QuantumCalibration cal{};
    cal.gain_mev_per_unit = 1.0f;
    cal.nominal_bath_temp_mk = 80.0f;
    cal.max_temp_drift_mk = 5.0f;
    QuantumDetectorSource src(cal);

    std::vector<QuantumEventSample> samples(16);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        samples[i].timestamp_ns = 5000ULL + i * 20ULL;
        samples[i].sequence_id = i;
        samples[i].pixel_id = static_cast<std::uint32_t>(i);
        samples[i].pulse_integral = 15.0f;
        samples[i].arrival_ps = 30.0f;
        samples[i].bath_temp_mk = 80.5f;
        samples[i].kind = CryoDetectorKind::SNSPD;
        samples[i].coincidence_count = 2;
        if (i % 4 == 0) {
            samples[i].health_flags = kCryoQuenchDetected;
        }
    }

    SpscRingBuffer<EventFrame, 64> ring;
    EXPECT_EQ(src.ingest_batch(std::span<const QuantumEventSample>(samples), ring), 16U);

    std::vector<EventFrame> popped(16);
    EXPECT_EQ(ring.try_pop_batch(std::span<EventFrame>(popped)), 16U);

    const TriggerConfig cfg = snspd_coincidence_trigger(2, 100.0f);
    std::vector<EventFrame> filtered(16);
    const std::size_t kept = filter_events(popped.data(), popped.size(), cfg, filtered.data());
    EXPECT_EQ(kept, 12U); // 4 quenched events vetoed

    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path out_path =
        std::filesystem::temp_directory_path() /
        ("chronoring_quantum_" + std::to_string(now) + ".crng");

    PipelineConfig pcfg;
    pcfg.output_path = out_path.string();
    pcfg.trigger = cfg;
    pcfg.enable_compression = true;

    TelemetryPipeline pipe(pcfg);
    ASSERT_TRUE(pipe.start());
    for (const auto& s : samples) {
        ASSERT_TRUE(src.ingest_one(s, pipe));
    }
    pipe.stop();

    const PipelineStats st = pipe.stats();
    EXPECT_EQ(st.last_status, RingStatus::Ok);
    EXPECT_EQ(st.consumed_events, 16U);
    EXPECT_EQ(st.passed_events, 12U);
    EXPECT_EQ(st.rejected_events, 4U);

    std::error_code ec;
    std::filesystem::remove(out_path, ec);
}

} // namespace chronoring::test
