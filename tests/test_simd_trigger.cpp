#include "chronoring/columnar_sink.hpp"
#include "chronoring/event_frame.hpp"
#include "chronoring/simd_trigger.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <limits>
#include <random>
#include <span>
#include <vector>

namespace chronoring::test {

namespace {

std::vector<EventFrame> build_edge_and_random_corpus(std::size_t total) {
    std::vector<EventFrame> events(total);
    std::mt19937 rng(20261121u);
    std::uniform_real_distribution<float> energy_dist(-5.0f, 12000.0f);
    std::uniform_real_distribution<float> timing_dist(-400.0f, 400.0f);
    std::uniform_real_distribution<float> pileup_dist(0.0f, 1.0f);
    std::uniform_int_distribution<std::uint32_t> flags_dist(0u, 31u);
    std::uniform_int_distribution<std::uint16_t> mult_dist(0u, 65535u);
    std::uniform_int_distribution<std::uint16_t> trig_dist(0u, 15u);

    for (std::size_t i = 0; i < total; ++i) {
        EventFrame& ev = events[i];
        ev.timestamp_ns = 100'000ULL + i * 50ULL;
        ev.sequence_id = i;
        ev.detector_id = static_cast<std::uint32_t>(i % 32U);
        ev.channel_id = static_cast<std::uint32_t>(i % 512U);
        ev.energy_mev = energy_dist(rng);
        ev.timing_ps = timing_dist(rng);
        ev.pileup_score = pileup_dist(rng);
        ev.quality_flags = flags_dist(rng);
        ev.multiplicity = mult_dist(rng);
        ev.trigger_mask = trig_dist(rng);
    }

    auto make_base_pass = []() {
        EventFrame ev{};
        ev.energy_mev = 100.0f;
        ev.timing_ps = 10.0f;
        ev.pileup_score = 0.1f;
        ev.quality_flags = kFlagValid | kFlagCalibrated;
        ev.multiplicity = 2;
        ev.trigger_mask = 0x3u;
        return ev;
    };

    if (total >= 20) {
        events[0] = make_base_pass();
        events[0].energy_mev = 0.5f;
        events[1] = make_base_pass();
        events[1].energy_mev = 10000.0f;
        events[2] = make_base_pass();
        events[2].energy_mev = std::numeric_limits<float>::quiet_NaN();
        events[3] = make_base_pass();
        events[3].timing_ps = -250.0f;
        events[4] = make_base_pass();
        events[4].timing_ps = -0.0f;
        events[5] = make_base_pass();
        events[5].timing_ps = std::numeric_limits<float>::quiet_NaN();
        events[6] = make_base_pass();
        events[6].pileup_score = 0.5f;
        events[7] = make_base_pass();
        events[7].pileup_score = std::numeric_limits<float>::quiet_NaN();
        events[8] = make_base_pass();
        events[8].multiplicity = 32768u;
        events[9] = make_base_pass();
        events[9].multiplicity = 65535u;
        events[10] = make_base_pass();
        events[10].multiplicity = 0u;
        events[11] = make_base_pass();
        events[11].quality_flags = kFlagValid | kFlagVeto;
        events[12] = make_base_pass();
        events[12].energy_mev = std::numeric_limits<float>::infinity();
    }
    return events;
}

bool custom_detector_even_only(const EventFrame& ev, void* user_ctx) noexcept {
    const auto* divisor = static_cast<const std::uint32_t*>(user_ctx);
    return (ev.detector_id % (*divisor)) == 0u;
}

} // namespace

TEST(SimdTriggerTest, BackendDetectionProducesValidNames) {
    const SimdBackend auto_be = active_backend(SimdBackend::Auto);
    EXPECT_NE(std::string(backend_name(auto_be)), "Unknown");
    EXPECT_EQ(active_backend(SimdBackend::Scalar), SimdBackend::Scalar);
}

TEST(SimdTriggerTest, BitExactEquivalenceAoSAndBitmask) {
    constexpr std::size_t kCount = 4099;
    const auto corpus = build_edge_and_random_corpus(kCount);

    std::uint32_t mod = 2;
    std::vector<TriggerConfig> configs;
    configs.push_back(TriggerConfig{});

    TriggerConfig strict_cfg{};
    strict_cfg.min_energy_mev = 50.0f;
    strict_cfg.max_energy_mev = 5000.0f;
    strict_cfg.max_abs_timing_ps = 120.0f;
    strict_cfg.max_pileup_score = 0.35f;
    strict_cfg.min_multiplicity = 32768u;
    strict_cfg.required_trigger_mask = 0x5u;
    configs.push_back(strict_cfg);

    TriggerConfig custom_cfg = strict_cfg;
    custom_cfg.min_multiplicity = 1;
    custom_cfg.custom_predicate = &custom_detector_even_only;
    custom_cfg.custom_user_ctx = &mod;
    configs.push_back(custom_cfg);

    const std::size_t words = (kCount + 63u) / 64u;

    for (const TriggerConfig& cfg : configs) {
        std::vector<EventFrame> out_scalar(kCount);
        std::vector<EventFrame> out_sse(kCount);
        std::vector<EventFrame> out_avx(kCount);
        std::vector<EventFrame> out_neon(kCount);
        std::vector<std::uint64_t> mask_scalar(words, 0);
        std::vector<std::uint64_t> mask_sse(words, 0);
        std::vector<std::uint64_t> mask_avx(words, 0);
        std::vector<std::uint64_t> mask_neon(words, 0);

        const std::size_t n_scalar =
            filter_events_scalar(corpus.data(), kCount, cfg, out_scalar.data());
        const std::size_t b_scalar =
            evaluate_batch_bitmask_scalar(corpus.data(), kCount, cfg, mask_scalar.data());
        EXPECT_EQ(n_scalar, b_scalar);

        const std::size_t n_sse =
            filter_events_sse42(corpus.data(), kCount, cfg, out_sse.data());
        const std::size_t b_sse =
            evaluate_batch_bitmask_sse42(corpus.data(), kCount, cfg, mask_sse.data());
        EXPECT_EQ(n_sse, n_scalar);
        EXPECT_EQ(b_sse, b_scalar);
        EXPECT_EQ(mask_sse, mask_scalar);
        EXPECT_EQ(std::memcmp(out_sse.data(), out_scalar.data(),
                              n_scalar * sizeof(EventFrame)),
                  0);

        const std::size_t n_avx =
            filter_events_avx2(corpus.data(), kCount, cfg, out_avx.data());
        const std::size_t b_avx =
            evaluate_batch_bitmask_avx2(corpus.data(), kCount, cfg, mask_avx.data());
        EXPECT_EQ(n_avx, n_scalar);
        EXPECT_EQ(b_avx, b_scalar);
        EXPECT_EQ(mask_avx, mask_scalar);
        EXPECT_EQ(std::memcmp(out_avx.data(), out_scalar.data(),
                              n_scalar * sizeof(EventFrame)),
                  0);

        const std::size_t n_neon =
            filter_events_neon(corpus.data(), kCount, cfg, out_neon.data());
        const std::size_t b_neon =
            evaluate_batch_bitmask_neon(corpus.data(), kCount, cfg, mask_neon.data());
        EXPECT_EQ(n_neon, n_scalar);
        EXPECT_EQ(b_neon, b_scalar);
        EXPECT_EQ(mask_neon, mask_scalar);
        EXPECT_EQ(std::memcmp(out_neon.data(), out_scalar.data(),
                              n_scalar * sizeof(EventFrame)),
                  0);
    }
}

TEST(SimdTriggerTest, BitExactEquivalenceColumnarPageSoAAndCompaction) {
    const auto full_corpus = build_edge_and_random_corpus(kColumnarPageEvents);
    std::uint32_t mod = 3;

    for (std::size_t count : {std::size_t{777}, kColumnarPageEvents}) {
        ColumnarPage page{};
        transpose_aos_to_soa(full_corpus.data(), count, page);

        TriggerConfig cfg{};
        cfg.required_trigger_mask = 0x2u;
        cfg.min_multiplicity = 100u;
        cfg.custom_predicate = &custom_detector_even_only;
        cfg.custom_user_ctx = &mod;

        const std::size_t words = (count + 63u) / 64u;
        std::vector<std::uint64_t> mask_aos(words, 0);
        std::vector<std::uint64_t> mask_soa_scalar(words, 0);
        std::vector<std::uint64_t> mask_soa_sse(words, 0);
        std::vector<std::uint64_t> mask_soa_avx(words, 0);

        const std::size_t n_aos =
            evaluate_batch_bitmask_scalar(full_corpus.data(), count, cfg, mask_aos.data());
        const std::size_t n_soa_scalar =
            evaluate_columnar_page_scalar(page, cfg, mask_soa_scalar.data());
        const std::size_t n_soa_sse =
            evaluate_columnar_page_sse42(page, cfg, mask_soa_sse.data());
        const std::size_t n_soa_avx =
            evaluate_columnar_page_avx2(page, cfg, mask_soa_avx.data());

        EXPECT_EQ(n_soa_scalar, n_aos);
        EXPECT_EQ(n_soa_sse, n_aos);
        EXPECT_EQ(n_soa_avx, n_aos);
        EXPECT_EQ(mask_soa_scalar, mask_aos);
        EXPECT_EQ(mask_soa_sse, mask_aos);
        EXPECT_EQ(mask_soa_avx, mask_aos);

        for (SimdBackend bk : {SimdBackend::AVX512, SimdBackend::SVE2,
                               SimdBackend::RVV, SimdBackend::WasmSimd128}) {
            std::vector<std::uint64_t> mask_bk(words, 0);
            const std::size_t n_bk = evaluate_columnar_page(page, cfg, mask_bk.data(), bk);
            EXPECT_EQ(n_bk, n_aos) << "Backend: " << backend_name(bk);
            EXPECT_EQ(mask_bk, mask_aos) << "Backend: " << backend_name(bk);
        }

        std::vector<EventFrame> compacted_aos(count);
        std::vector<EventFrame> compacted_soa(count);
        const std::size_t kept_aos =
            filter_events(std::span<const EventFrame>(full_corpus.data(), count), cfg,
                          std::span<EventFrame>(compacted_aos));
        const std::size_t kept_soa =
            filter_columnar_page_to_aos(page, cfg, compacted_soa.data());
        EXPECT_EQ(kept_soa, kept_aos);
        for (std::size_t i = 0; i < kept_aos; ++i) {
            EXPECT_EQ(compacted_soa[i].sequence_id, compacted_aos[i].sequence_id);
        }
    }
}

TEST(SimdTriggerTest, RejectReasonDiagnosticsAndJsonRoundTrip) {
    TriggerConfig cfg{};
    cfg.required_trigger_mask = 0x4u;
    cfg.min_multiplicity = 3;

    EventFrame good{};
    good.energy_mev = 50.0f;
    good.timing_ps = -10.0f;
    good.pileup_score = 0.2f;
    good.quality_flags = kFlagValid;
    good.multiplicity = 3;
    good.trigger_mask = 0x4u;
    EXPECT_EQ(diagnose_reject_reasons(good, cfg), kRejectNone);

    EventFrame bad = good;
    bad.energy_mev = 0.1f;
    EXPECT_EQ(diagnose_reject_reasons(bad, cfg), kRejectMinEnergy);

    bad = good;
    bad.energy_mev = 50000.0f;
    EXPECT_EQ(diagnose_reject_reasons(bad, cfg), kRejectMaxEnergy);

    bad = good;
    bad.timing_ps = -300.0f;
    EXPECT_EQ(diagnose_reject_reasons(bad, cfg), kRejectTiming);

    bad = good;
    bad.pileup_score = 0.9f;
    EXPECT_EQ(diagnose_reject_reasons(bad, cfg), kRejectPileup);

    bad = good;
    bad.quality_flags = kFlagNone;
    EXPECT_EQ(diagnose_reject_reasons(bad, cfg), kRejectRequiredFlags);

    bad = good;
    bad.quality_flags = kFlagValid | kFlagVeto;
    EXPECT_EQ(diagnose_reject_reasons(bad, cfg), kRejectForbiddenFlags);

    bad = good;
    bad.multiplicity = 1;
    bad.trigger_mask = 0x1u;
    EXPECT_EQ(diagnose_reject_reasons(bad, cfg),
              static_cast<std::uint8_t>(kRejectMultiplicity | kRejectTriggerMask));

    std::array<EventFrame, 2> pair{good, bad};
    std::array<std::uint8_t, 2> reasons{};
    EXPECT_EQ(diagnose_batch_reject_reasons(std::span<const EventFrame>(pair), cfg,
                                            std::span<std::uint8_t>(reasons)),
              1U);
    EXPECT_EQ(reasons[0], kRejectNone);
    EXPECT_NE(reasons[1], kRejectNone);

    const std::string json = to_json(cfg);
    TriggerConfig parsed{};
    EXPECT_TRUE(from_json(json, parsed));
    EXPECT_FLOAT_EQ(parsed.min_energy_mev, cfg.min_energy_mev);
    EXPECT_FLOAT_EQ(parsed.max_energy_mev, cfg.max_energy_mev);
    EXPECT_FLOAT_EQ(parsed.max_abs_timing_ps, cfg.max_abs_timing_ps);
    EXPECT_FLOAT_EQ(parsed.max_pileup_score, cfg.max_pileup_score);
    EXPECT_EQ(parsed.required_flags, cfg.required_flags);
    EXPECT_EQ(parsed.forbidden_flags, cfg.forbidden_flags);
    EXPECT_EQ(parsed.min_multiplicity, cfg.min_multiplicity);
    EXPECT_EQ(parsed.required_trigger_mask, cfg.required_trigger_mask);
}

TEST(SimdTriggerTest, BackendNamesAndDetectionQueries) {
    EXPECT_STREQ(backend_name(SimdBackend::AVX512), "AVX-512");
    EXPECT_STREQ(backend_name(SimdBackend::SVE2), "SVE2");
    EXPECT_STREQ(backend_name(SimdBackend::RVV), "RVV");
    EXPECT_STREQ(backend_name(SimdBackend::WasmSimd128), "WASM-SIMD128");
    (void)cpu_supports_avx512();
    (void)cpu_supports_sve2();
    (void)cpu_supports_rvv();
    (void)cpu_supports_wasm_simd128();
    EXPECT_NE(backend_name(active_backend(SimdBackend::Auto)), nullptr);
}

} // namespace chronoring::test
