#include "chronoring/columnar_sink.hpp"
#include "chronoring/event_frame.hpp"
#include "chronoring/hw_offload.hpp"
#include "chronoring/soa_pipeline.hpp"
#include "chronoring/trigger_jit.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <random>
#include <thread>
#include <vector>

namespace chronoring::test {

namespace {

ColumnarPage make_mixed_soa_page(std::size_t count) {
    ColumnarPage page{};
    page.header.magic = kColumnarPageMagic;
    page.header.version = kColumnarFormatVersion;
    page.header.capacity = static_cast<std::uint32_t>(kColumnarPageEvents);

    SoaBatchBuilder builder(&page);
    std::mt19937 rng(0x5EED1234u);
    std::uniform_real_distribution<float> e_dist(0.0f, 150.0f);
    std::uniform_real_distribution<float> t_dist(-300.0f, 300.0f);
    std::uniform_real_distribution<float> pu_dist(0.0f, 1.0f);

    for (std::size_t i = 0; i < count; ++i) {
        const std::uint32_t flags =
            (i % 7U == 0U) ? (kFlagValid | kFlagVeto)
                           : ((i % 5U == 0U) ? kFlagNone : (kFlagValid | kFlagCalibrated));
        EXPECT_TRUE(builder.append_fields(
            10'000ULL + i * 20ULL,
            i,
            static_cast<std::uint32_t>(i % 4U),
            static_cast<std::uint32_t>(i % 128U),
            e_dist(rng),
            t_dist(rng),
            pu_dist(rng),
            flags,
            static_cast<std::uint16_t>((i % 4U) + 1U),
            static_cast<std::uint16_t>(1U << (i % 3U))));
    }
    return page;
}

} // namespace

TEST(TriggerJitTest, DslCompilationAndBitExactSimdEquivalence) {
    // Reject empty or malformed DSL expressions cleanly
    EXPECT_FALSE(compile_trigger_dsl("").has_value());
    EXPECT_FALSE(compile_trigger_dsl("unknown_col >= 1.0").has_value());
    EXPECT_FALSE(compile_trigger_dsl("energy >=").has_value());
    EXPECT_FALSE(compile_trigger_dsl("(energy >= 10.0").has_value());

    const std::string dsl =
        "energy >= 15.0 && abs(timing) <= 180.0 && (flags & 0x3) == 0x3 && "
        "(flags & 0xC) == 0 && (detector_id == 0 || detector_id == 2) && "
        "!(pileup > 0.65) && multiplicity >= 2";

    for (SimdBackend bk : {SimdBackend::Scalar, SimdBackend::AVX2, SimdBackend::AVX512, SimdBackend::Auto}) {
        const auto prog = compile_trigger_dsl(dsl, bk);
        ASSERT_TRUE(prog.has_value()) << "Failed to compile DSL for backend";

        for (std::size_t count : {std::size_t{193}, kColumnarPageEvents}) {
            const ColumnarPage page = make_mixed_soa_page(count);
            const std::size_t words = (count + 63u) / 64u;
            std::vector<std::uint64_t> mask_scalar(words, 0);
            std::vector<std::uint64_t> mask_jit(words, 0);

            const std::size_t n_scalar =
                evaluate_trigger_program_scalar(*prog, page, mask_scalar.data());
            const std::size_t n_jit =
                evaluate_trigger_program(*prog, page, mask_jit.data());

            EXPECT_EQ(n_jit, n_scalar);
            EXPECT_EQ(mask_jit, mask_scalar);
            EXPECT_GT(n_jit, 0U);
            EXPECT_LT(n_jit, count);
        }
    }
}

TEST(TriggerJitTest, SoaDataflowPipelineWithJitAndHwOffload) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path p_jit =
        std::filesystem::temp_directory_path() /
        ("chronoring_soa_jit_" + std::to_string(now) + ".crng");
    const std::filesystem::path p_hw =
        std::filesystem::temp_directory_path() /
        ("chronoring_soa_hw_" + std::to_string(now) + ".crng");

    // 1. Run pure-SoA pipeline driven by JIT-compiled DSL trigger
    const auto prog = compile_trigger_dsl(
        "energy >= 20.0 && abs(timing) <= 200.0 && (flags & 1) == 1");
    ASSERT_TRUE(prog.has_value());

    {
        SoaPipelineConfig cfg;
        cfg.output_path = p_jit.string();
        cfg.jit_program = &(*prog);
        cfg.enable_compression = true;

        SoaDataflowPipeline pipe(cfg);
        ASSERT_TRUE(pipe.start());

        for (int p = 0; p < 4; ++p) {
            std::size_t idx = 0;
            ColumnarPage* dst = nullptr;
            while ((dst = pipe.acquire_input_page(idx)) == nullptr) {
                std::this_thread::yield();
            }
            SoaBatchBuilder builder(dst);
            for (std::size_t i = 0; i < 512; ++i) {
                const float e = (i % 2 == 0) ? 50.0f : 5.0f;
                ASSERT_TRUE(builder.append_fields(
                    1000ULL + i, static_cast<std::uint64_t>(p) * 512ULL + i,
                    0, 0, e, 10.0f, 0.1f, kFlagValid | kFlagCalibrated, 1, 0));
            }
            ASSERT_TRUE(pipe.submit_input_page(idx));
        }
        pipe.stop();

        EXPECT_EQ(pipe.last_status(), RingStatus::Ok);
        EXPECT_EQ(pipe.sink_errors(), 0U);
        EXPECT_EQ(pipe.consumed_events(), 2048U);
        EXPECT_EQ(pipe.passed_events(), 1024U);

        ColumnarReader reader(p_jit.string());
        ASSERT_TRUE(reader.is_open());
        EXPECT_TRUE(reader.verify_all_pages());
        EXPECT_EQ(reader.total_events(), 1024U);
        reader.close();
    }

    // 2. Run pure-SoA pipeline driven by OffloadStageRouter (simulated GPU + fallback)
    {
        SimulatedAcceleratorOffloadDevice gpu(OffloadBackendKind::CudaHipGpu, "Mock-CUDA", true);
        SimulatedAcceleratorOffloadDevice offline_fpga(OffloadBackendKind::FpgaOpenCl, "Offline-FPGA", false);
        OffloadStageRouter router;
        router.register_device(&gpu);
        router.register_device(&offline_fpga);
        router.set_preferred(OffloadBackendKind::CudaHipGpu);
        EXPECT_TRUE(router.is_available(OffloadBackendKind::CudaHipGpu));
        EXPECT_FALSE(router.is_available(OffloadBackendKind::FpgaOpenCl));

        IoUringOffloadDevice io_uring;
        (void)io_uring.capabilities();

        SoaPipelineConfig cfg;
        cfg.output_path = p_hw.string();
        cfg.offload_router = &router;
        cfg.trigger.min_energy_mev = 10.0f;

        SoaDataflowPipeline pipe(cfg);
        ASSERT_TRUE(pipe.start());

        std::size_t idx = 0;
        ColumnarPage* dst = pipe.acquire_input_page(idx);
        ASSERT_NE(dst, nullptr);
        SoaBatchBuilder builder(dst);
        for (std::size_t i = 0; i < 64; ++i) {
            ASSERT_TRUE(builder.append_fields(
                2000ULL + i, i, 0, 0, (i % 4 == 0) ? 2.0f : 30.0f,
                10.0f, 0.1f, kFlagValid, 1, 0));
        }
        ASSERT_TRUE(pipe.submit_input_page(idx));
        pipe.stop();

        EXPECT_EQ(pipe.last_status(), RingStatus::Ok);
        EXPECT_EQ(pipe.offload_executions(), 1U);
        EXPECT_EQ(pipe.passed_events(), 48U);

        // Verify unavailable preferred device falls back cleanly to CpuSimd
        router.set_preferred(OffloadBackendKind::FpgaOpenCl);
        const ColumnarPage ref_page = make_mixed_soa_page(64);
        std::array<std::uint64_t, kColumnarPageEvents / 64u> words{};
        const OffloadResult res = router.execute_page(ref_page, TriggerConfig{}, words.data());
        EXPECT_EQ(res.status, RingStatus::Ok);
        EXPECT_EQ(res.executed_on, OffloadBackendKind::CpuSimd);
    }

    std::error_code ec;
    std::filesystem::remove(p_jit, ec);
    std::filesystem::remove(p_hw, ec);
}

} // namespace chronoring::test
