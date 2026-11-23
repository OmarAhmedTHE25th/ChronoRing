#include "chronoring/columnar_sink.hpp"
#include "chronoring/event_frame.hpp"
#include "chronoring/pipeline.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
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
    // Standard ASCII check value ("123456789")
    constexpr char kVector[] = "123456789";
    EXPECT_EQ(crc32c_sw(kVector, 9), 0xE3069283u);
    EXPECT_EQ(crc32c_hw(kVector, 9), 0xE3069283u);
    EXPECT_EQ(crc32c(kVector, 9), 0xE3069283u);

    // RFC 3720 Appendix B.4 iSCSI CRC32C Known-Answer Tests
    std::array<std::uint8_t, 32> zeros{};
    EXPECT_EQ(crc32c_sw(zeros.data(), zeros.size()), 0x8A9136AAu);
    EXPECT_EQ(crc32c_hw(zeros.data(), zeros.size()), 0x8A9136AAu);

    std::array<std::uint8_t, 32> ones{};
    ones.fill(0xFFu);
    EXPECT_EQ(crc32c_sw(ones.data(), ones.size()), 0x62A8AB43u);
    EXPECT_EQ(crc32c_hw(ones.data(), ones.size()), 0x62A8AB43u);

    std::array<std::uint8_t, 32> asc{};
    std::array<std::uint8_t, 32> desc{};
    for (std::size_t i = 0; i < 32; ++i) {
        asc[i] = static_cast<std::uint8_t>(i);
        desc[i] = static_cast<std::uint8_t>(31U - i);
    }
    EXPECT_EQ(crc32c_sw(asc.data(), asc.size()), 0x46DD794Eu);
    EXPECT_EQ(crc32c_hw(asc.data(), asc.size()), 0x46DD794Eu);
    EXPECT_EQ(crc32c_sw(desc.data(), desc.size()), 0x113FDB5Cu);
    EXPECT_EQ(crc32c_hw(desc.data(), desc.size()), 0x113FDB5Cu);

    const std::array<std::uint8_t, 48> iscsi_read_pdu{
        0x01U, 0xC0U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x14U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x04U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x14U, 0x00U, 0x00U, 0x00U, 0x18U,
        0x28U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    };
    EXPECT_EQ(crc32c_sw(iscsi_read_pdu.data(), iscsi_read_pdu.size()), 0xD9963A56u);
    EXPECT_EQ(crc32c_hw(iscsi_read_pdu.data(), iscsi_read_pdu.size()), 0xD9963A56u);

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
    EXPECT_EQ(page.header.flags, kColAll);
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

    page.energy_mev[42] += 0.125f;
    EXPECT_FALSE(verify_page_crc(page));

    finalize_page_crc(page);
    EXPECT_TRUE(verify_page_crc(page));
    page.header.max_timestamp_ns ^= 1ULL;
    EXPECT_FALSE(verify_page_crc(page));
}

TEST(ColumnarSinkTest, MmapSinkAutoGrowPartialPagesAndTruncationDetection) {
    const std::filesystem::path path = temp_file_path("sink_grow");
    constexpr std::size_t kTotal = 5000;

    {
        ColumnarSink sink(path.string(), 4);
        ASSERT_TRUE(sink.is_open()) << sink.last_error();

        std::vector<EventFrame> batch(250);
        for (std::size_t base = 0; base < kTotal; base += batch.size()) {
            for (std::size_t i = 0; i < batch.size(); ++i) {
                batch[i] = make_test_event(base + i);
            }
            EXPECT_EQ(sink.write_batch(std::span<const EventFrame>(batch)), batch.size());
        }
        sink.close();
        EXPECT_EQ(sink.events_written(), kTotal);
        EXPECT_EQ(sink.pages_written(), 5U);
        EXPECT_EQ(sink.last_status(), RingStatus::Ok);
    }

    {
        ColumnarReader reader(path.string());
        ASSERT_TRUE(reader.is_open()) << reader.last_error();
        EXPECT_EQ(reader.num_pages(), 5U);
        EXPECT_EQ(reader.total_events(), kTotal);
        EXPECT_TRUE(reader.verify_all_pages());
        EXPECT_EQ(reader.page(0).header.event_count, kColumnarPageEvents);
        EXPECT_EQ(reader.page(4).header.event_count, kTotal - 4U * kColumnarPageEvents);

        // Index and seek tests
        const auto index = reader.page_index();
        ASSERT_EQ(index.size(), 5U);
        EXPECT_EQ(index[2].first_sequence_id, 2048U);
        EXPECT_EQ(index[2].last_sequence_id, 3071U);

        const auto by_ts = reader.find_pages_by_timestamp(
            index[2].min_timestamp_ns, index[2].max_timestamp_ns);
        ASSERT_EQ(by_ts.size(), 1U);
        EXPECT_EQ(by_ts[0], 2U);

        const auto by_seq = reader.find_page_by_sequence(2500);
        ASSERT_TRUE(by_seq.has_value());
        EXPECT_EQ(*by_seq, 2U);
        EXPECT_FALSE(reader.find_page_by_sequence(999'999).has_value());

        // Offline SoA replay
        std::uint64_t scanned = 0;
        const auto replay = reader.replay_filtered(TriggerConfig{}, SimdBackend::Auto, &scanned);
        ASSERT_TRUE(replay.has_value());
        EXPECT_EQ(scanned, kTotal);
        EXPECT_EQ(*replay, kTotal);
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
        const auto replay = reader.replay_filtered(TriggerConfig{});
        EXPECT_FALSE(replay.has_value());
        EXPECT_EQ(replay.error(), RingStatus::CorruptedPage);
    }

    // Truncate file by 17 bytes and confirm alignment error detection
    std::filesystem::resize_file(path, std::filesystem::file_size(path) - 17U);
    {
        ColumnarReader reader(path.string());
        ASSERT_TRUE(reader.is_open());
        EXPECT_FALSE(reader.verify_all_pages());
        EXPECT_FALSE(reader.replay_filtered(TriggerConfig{}).has_value());
    }

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(ColumnarSinkTest, ColumnProjectionAndConcurrentReaderWhileGrowing) {
    const std::filesystem::path path = temp_file_path("proj_concurrent");

    // Write 6 pages (initial capacity 4 -> triggers expansion) and read flushed pages before close()
    ColumnarSink sink(path.string(), 4,
                      static_cast<std::uint16_t>(kColTimestamps | kColKinematics));
    ASSERT_TRUE(sink.is_open()) << sink.last_error();

    constexpr std::size_t kFirstWave = 6U * kColumnarPageEvents;
    for (std::size_t i = 0; i < kFirstWave; ++i) {
        ASSERT_TRUE(sink.write(make_test_event(i)));
    }
    ASSERT_TRUE(sink.flush());
    EXPECT_EQ(sink.pages_written(), 6U);

    {
        ColumnarReader live_reader(path.string());
        ASSERT_TRUE(live_reader.is_open());
        EXPECT_GE(live_reader.num_pages(), 6U);
        for (std::size_t p = 0; p < 6U; ++p) {
            EXPECT_TRUE(verify_page_crc(live_reader.page(p)));
            EXPECT_EQ(live_reader.page(p).header.flags,
                      static_cast<std::uint16_t>(kColTimestamps | kColKinematics));
            // Projected-out positions remain zero
            EXPECT_FLOAT_EQ(live_reader.page(p).pos_x_mm[0], 0.0f);
            EXPECT_FLOAT_EQ(live_reader.page(p).energy_mev[0],
                            make_test_event(p * kColumnarPageEvents).energy_mev);
        }
    }

    // Write partial tail page and close
    for (std::size_t i = 0; i < 137; ++i) {
        ASSERT_TRUE(sink.write(make_test_event(kFirstWave + i)));
    }
    sink.close();

    ColumnarReader final_reader(path.string());
    ASSERT_TRUE(final_reader.is_open());
    EXPECT_EQ(final_reader.num_pages(), 7U);
    EXPECT_EQ(final_reader.total_events(), kFirstWave + 137U);
    EXPECT_TRUE(final_reader.verify_all_pages());
    final_reader.close();

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(ColumnarSinkTest, EndToEndPipelineSoAStagingConfigAndPrometheus) {
    const std::filesystem::path path = temp_file_path("pipeline");
    constexpr std::size_t kEvents = 20'000;

    PipelineConfig cfg;
    cfg.output_path = path.string();
    cfg.simd_backend = SimdBackend::Auto;
    cfg.batch_size = 256;
    cfg.use_soa_staging = true;

    const std::string cfg_json = to_json(cfg);
    PipelineConfig parsed_cfg{};
    ASSERT_TRUE(from_json(cfg_json, parsed_cfg));
    EXPECT_EQ(parsed_cfg.output_path, cfg.output_path);
    EXPECT_EQ(parsed_cfg.batch_size, cfg.batch_size);
    EXPECT_EQ(parsed_cfg.use_soa_staging, cfg.use_soa_staging);

    TelemetryPipeline pipeline(parsed_cfg);
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
    EXPECT_EQ(stats.last_status, RingStatus::Ok);
    EXPECT_EQ(stats.sink_errors, 0U);
    EXPECT_EQ(stats.submitted_events, kEvents);
    EXPECT_EQ(stats.consumed_events, kEvents);
    EXPECT_EQ(stats.passed_events, expected_passed);
    EXPECT_EQ(stats.rejected_events, kEvents - expected_passed);
    EXPECT_LE(stats.min_ns, stats.p50_ns);
    EXPECT_LE(stats.p50_ns, stats.p90_ns);
    EXPECT_LE(stats.p90_ns, stats.p99_ns);
    EXPECT_LE(stats.p99_ns, stats.p99_9_ns);
    EXPECT_LE(stats.p99_9_ns, stats.max_ns);

    const std::string prom = to_prometheus_text(stats);
    EXPECT_NE(prom.find("chronoring_events_submitted_total 20000"), std::string::npos);
    EXPECT_NE(prom.find("chronoring_latency_ns{quantile=\"0.99\"}"), std::string::npos);

    ColumnarReader reader(path.string());
    ASSERT_TRUE(reader.is_open()) << reader.last_error();
    EXPECT_EQ(reader.total_events(), expected_passed);
    EXPECT_TRUE(reader.verify_all_pages());
    reader.close();

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(ColumnarSinkTest, Lz4CompressionRoundTripAndCorruptionDetection) {
    const std::filesystem::path path = temp_file_path("lz4_comp");
    constexpr std::size_t kTotal = 3000;

    std::size_t compressed_file_bytes = 0;
    {
        ColumnarSink sink(path.string(), 4, kColAll, true);
        ASSERT_TRUE(sink.is_open()) << sink.last_error();
        EXPECT_TRUE(sink.compression_enabled());

        std::vector<EventFrame> batch(500);
        for (std::size_t base = 0; base < kTotal; base += batch.size()) {
            for (std::size_t i = 0; i < batch.size(); ++i) {
                batch[i] = make_test_event(base + i, true);
            }
            EXPECT_EQ(sink.write_batch(std::span<const EventFrame>(batch)), batch.size());
        }
        sink.close();
        compressed_file_bytes = sink.bytes_written();
        EXPECT_EQ(sink.pages_written(), 3U);
        EXPECT_LT(compressed_file_bytes, 3U * sizeof(ColumnarPage));
    }

    {
        ColumnarReader reader(path.string());
        ASSERT_TRUE(reader.is_open()) << reader.last_error();
        EXPECT_EQ(reader.num_pages(), 3U);
        EXPECT_EQ(reader.total_events(), kTotal);
        EXPECT_TRUE(reader.verify_all_pages());

        ColumnarPage page{};
        ASSERT_TRUE(reader.read_page(0, page));
        EXPECT_EQ(page.header.event_count, kColumnarPageEvents);
        EXPECT_EQ(page.sequence_id[42], 42U);

        std::uint64_t scanned = 0;
        const auto replay = reader.replay_filtered(TriggerConfig{}, SimdBackend::Auto, &scanned);
        ASSERT_TRUE(replay.has_value());
        EXPECT_EQ(scanned, kTotal);
        EXPECT_EQ(*replay, kTotal);
    }

    // Corrupt a compressed payload byte on page 1
    {
        std::fstream fs(path, std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(fs.is_open());
        fs.seekg(static_cast<std::streamoff>(sizeof(ColumnarPageHeader) + 8U));
        char b = 0;
        fs.read(&b, 1);
        b ^= 0x3C;
        fs.seekp(static_cast<std::streamoff>(sizeof(ColumnarPageHeader) + 8U));
        fs.write(&b, 1);
    }

    {
        ColumnarReader reader(path.string());
        ASSERT_TRUE(reader.is_open());
        std::size_t bad_idx = 999;
        EXPECT_FALSE(reader.verify_all_pages(&bad_idx));
        EXPECT_EQ(bad_idx, 0U);
    }

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(ColumnarSinkTest, DualWriteAdaptiveBatchingAndRejectReasons) {
    const std::filesystem::path primary = temp_file_path("dual_p");
    const std::filesystem::path secondary = temp_file_path("dual_s");

    PipelineConfig cfg;
    cfg.output_path = primary.string();
    cfg.secondary_output_path = secondary.string();
    cfg.enable_compression = true;
    cfg.adaptive_batching = true;
    cfg.min_batch_size = 16;
    cfg.max_batch_size = 512;
    cfg.trigger.min_energy_mev = 5.0f;

    TelemetryPipeline pipeline(cfg);
    ASSERT_TRUE(pipeline.start());

    constexpr std::size_t kEvents = 4000;
    for (std::size_t i = 0; i < kEvents; ++i) {
        const bool pass = (i % 4U) != 0U;
        pipeline.submit_wait(make_test_event(i, pass));
    }
    pipeline.stop();

    const PipelineStats stats = pipeline.stats();
    EXPECT_EQ(stats.last_status, RingStatus::Ok);
    EXPECT_EQ(stats.sink_errors, 0U);
    EXPECT_EQ(stats.passed_events, 3000U);
    EXPECT_EQ(stats.rejected_events, 1000U);
    EXPECT_EQ(stats.reject_reasons[0], 1000U); // energy_low

    const std::string prom = to_prometheus_text(stats);
    EXPECT_NE(prom.find("chronoring_reject_reason_total{reason=\"energy_low\"} 1000"),
              std::string::npos);
    EXPECT_NE(prom.find("chronoring_last_status{status=\"Ok\"} 0"), std::string::npos);

    ColumnarReader r1(primary.string());
    ColumnarReader r2(secondary.string());
    ASSERT_TRUE(r1.is_open());
    ASSERT_TRUE(r2.is_open());
    EXPECT_TRUE(r1.verify_all_pages());
    EXPECT_TRUE(r2.verify_all_pages());
    EXPECT_EQ(r1.total_events(), 3000U);
    EXPECT_EQ(r2.total_events(), 3000U);
    r1.close();
    r2.close();

    std::error_code ec;
    std::filesystem::remove(primary, ec);
    std::filesystem::remove(secondary, ec);
}

} // namespace chronoring::test
