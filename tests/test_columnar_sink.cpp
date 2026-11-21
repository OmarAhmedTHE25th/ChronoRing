#include "chronoring/columnar_sink.hpp"
#include "chronoring/event_frame.hpp"
#include "chronoring/pipeline.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace chronoring::test {

namespace {

EventFrame make_test_event(std::uint64_t seq, bool pass = true) noexcept {
    EventFrame f{};
    f.timestamp_ns = 1'000'000ULL + seq * 100ULL;
    f.sequence_id = seq;
    f.detector_id = static_cast<std::uint32_t>(seq % 16U);
    f.channel_id = static_cast<std::uint32_t>(seq % 256U);
    f.energy_mev = pass ? (25.0f + static_cast<float>(seq % 500U)) : 0.05f;
    f.timing_ps = static_cast<float>(static_cast<int>(seq % 200U) - 100);
    f.pos_x_mm = 1.5f * static_cast<float>(seq % 40U);
    f.pos_y_mm = -2.25f * static_cast<float>(seq % 30U);
    f.pos_z_mm = 3.75f * static_cast<float>(seq % 50U);
    f.pileup_score = pass ? 0.10f : 0.90f;
    f.quality_flags = pass ? (kFlagValid | kFlagCalibrated) : kFlagVeto;
    f.multiplicity = 3;
    f.trigger_mask = 0x1u;
    return f;
}

std::filesystem::path temp_file_path(const char* name) {
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           (std::string("chronoring_") + name + "_" + std::to_string(tick) + ".crng");
}

} // namespace

TEST(ColumnarSinkTest, Crc32cStandardVectorAndHwSwEquivalence) {
    constexpr char kVector[] = "123456789";
    EXPECT_EQ(crc32c_sw(kVector, 9), 0xE3069283u);
    EXPECT_EQ(crc32c_hw(kVector, 9), 0xE3069283u);
    EXPECT_EQ(crc32c(kVector, 9), 0xE3069283u);

    std::vector<std::uint8_t> buf(1033);
    for (std::size_t i = 0; i < buf.size(); ++i) {
        buf[i] = static_cast<std::uint8_t>((i * 131U + 17U) & 0xFFU);
    }
    for (std::size_t offset = 0; offset < 8; ++offset) {
        for (std::size_t len = 0; len <= 128; ++len) {
            const std::uint32_t sw = crc32c_sw(buf.data() + offset, len, 0x12345678u);
            const std::uint32_t hw = crc32c_hw(buf.data() + offset, len, 0x12345678u);
            EXPECT_EQ(hw, sw);
        }
    }
}

TEST(ColumnarSinkTest, AosSoaRoundTripAndCrcCorruptionDetection) {
    constexpr std::size_t kPartial = 333;
    std::vector<EventFrame> src(kPartial);
    for (std::size_t i = 0; i < kPartial; ++i) {
        src[i] = make_test_event(i);
    }

    ColumnarPage page{};
    transpose_aos_to_soa(src.data(), kPartial, page);
    EXPECT_EQ(page.header.event_count, kPartial);
    EXPECT_TRUE(verify_page_crc(page));

    std::vector<EventFrame> dst(kPartial);
    EXPECT_EQ(transpose_soa_to_aos(page, dst.data(), dst.size()), kPartial);
    for (std::size_t i = 0; i < kPartial; ++i) {
        EXPECT_EQ(dst[i].timestamp_ns, src[i].timestamp_ns);
        EXPECT_EQ(dst[i].sequence_id, src[i].sequence_id);
        EXPECT_EQ(dst[i].detector_id, src[i].detector_id);
        EXPECT_FLOAT_EQ(dst[i].energy_mev, src[i].energy_mev);
        EXPECT_FLOAT_EQ(dst[i].pos_z_mm, src[i].pos_z_mm);
        EXPECT_EQ(dst[i].quality_flags, src[i].quality_flags);
    }

    // Corrupt a single payload value
    page.energy_mev[42] += 0.125f;
    EXPECT_FALSE(verify_page_crc(page));

    // Re-finalize then corrupt a header byte
    finalize_page_crc(page);
    EXPECT_TRUE(verify_page_crc(page));
    page.header.max_timestamp_ns ^= 1ULL;
    EXPECT_FALSE(verify_page_crc(page));
}

TEST(ColumnarSinkTest, MmapSinkAutoGrowAndFileCorruptionDetection) {
    const std::filesystem::path path = temp_file_path("sink_grow");
    constexpr std::size_t kTotal = 5000; // > 4 initial pages

    {
        ColumnarSink sink(path.string(), 4);
        ASSERT_TRUE(sink.is_open()) << sink.last_error();

        std::vector<EventFrame> batch(250);
        for (std::size_t base = 0; base < kTotal; base += batch.size()) {
            for (std::size_t i = 0; i < batch.size(); ++i) {
                batch[i] = make_test_event(base + i);
            }
            EXPECT_EQ(sink.write_batch(batch.data(), batch.size()), batch.size());
        }
        sink.close();
        EXPECT_EQ(sink.events_written(), kTotal);
        EXPECT_EQ(sink.pages_written(), 5U);
    }

    {
        ColumnarReader reader(path.string());
        ASSERT_TRUE(reader.is_open()) << reader.last_error();
        EXPECT_EQ(reader.num_pages(), 5U);
        EXPECT_EQ(reader.total_events(), kTotal);
        EXPECT_TRUE(reader.verify_all_pages());
        EXPECT_EQ(reader.page(0).header.event_count, kColumnarPageEvents);
        EXPECT_EQ(reader.page(4).header.event_count, kTotal - 4U * kColumnarPageEvents);
    }

    // Flip 1 byte inside page 2 payload on disk
    {
        std::fstream fs(path, std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(fs.is_open());
        const std::streamoff corrupt_offset =
            static_cast<std::streamoff>(2U * sizeof(ColumnarPage) + 256U);
        fs.seekg(corrupt_offset);
        char b = 0;
        fs.read(&b, 1);
        b ^= 0x55;
        fs.seekp(corrupt_offset);
        fs.write(&b, 1);
    }

    {
        ColumnarReader reader(path.string());
        ASSERT_TRUE(reader.is_open());
        std::size_t bad_page = 999;
        EXPECT_FALSE(reader.verify_all_pages(&bad_page));
        EXPECT_EQ(bad_page, 2U);
    }

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(ColumnarSinkTest, EndToEndTelemetryPipeline) {
    const std::filesystem::path path = temp_file_path("pipeline");
    constexpr std::size_t kEvents = 20'000;

    PipelineConfig cfg;
    cfg.output_path = path.string();
    cfg.simd_backend = SimdBackend::Auto;
    cfg.batch_size = 256;

    TelemetryPipeline pipeline(cfg);
    ASSERT_TRUE(pipeline.start()) << pipeline.sink_error();

    std::size_t expected_passed = 0;
    for (std::size_t i = 0; i < kEvents; ++i) {
        const bool pass = (i % 4U) != 0U;
        if (pass) {
            ++expected_passed;
        }
        pipeline.submit_wait(make_test_event(i, pass));
    }
    pipeline.stop();

    const PipelineStats stats = pipeline.stats();
    EXPECT_EQ(stats.submitted_events, kEvents);
    EXPECT_EQ(stats.consumed_events, kEvents);
    EXPECT_EQ(stats.passed_events, expected_passed);
    EXPECT_EQ(stats.rejected_events, kEvents - expected_passed);
    EXPECT_LE(stats.min_ns, stats.p50_ns);
    EXPECT_LE(stats.p50_ns, stats.p90_ns);
    EXPECT_LE(stats.p90_ns, stats.p99_ns);
    EXPECT_LE(stats.p99_ns, stats.p99_9_ns);
    EXPECT_LE(stats.p99_9_ns, stats.max_ns);

    ColumnarReader reader(path.string());
    ASSERT_TRUE(reader.is_open()) << reader.last_error();
    EXPECT_EQ(reader.total_events(), expected_passed);
    EXPECT_TRUE(reader.verify_all_pages());
    reader.close();

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

} // namespace chronoring::test
