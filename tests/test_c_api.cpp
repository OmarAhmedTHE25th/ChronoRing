#include "chronoring/c_api.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace {

std::filesystem::path temp_crng_path(const char* name) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           (std::string("chronoring_capi_") + name + "_" + std::to_string(now) + ".crng");
}

chronoring_event_frame_t make_c_event(uint64_t seq, float energy) {
    chronoring_event_frame_t f{};
    f.timestamp_ns = 10000ULL + seq * 100ULL;
    f.sequence_id = seq;
    f.detector_id = static_cast<uint32_t>(seq % 4U);
    f.channel_id = static_cast<uint32_t>(seq % 64U);
    f.energy_mev = energy;
    f.timing_ps = 10.0f;
    f.pileup_score = 0.05f;
    f.quality_flags = 0x3u;
    f.multiplicity = 2;
    return f;
}

} // namespace

TEST(CApiTest, VersionAndSpscRoundTrip) {
    EXPECT_STREQ(chronoring_version(), "1.2.0");

    chronoring_spsc_t* ring = chronoring_spsc_create();
    ASSERT_NE(ring, nullptr);
    const chronoring_event_frame_t ev = make_c_event(42, 12.5f);
    EXPECT_EQ(chronoring_spsc_try_push(ring, &ev), 1);
    EXPECT_EQ(chronoring_spsc_size(ring), 1U);

    chronoring_event_frame_t out{};
    EXPECT_EQ(chronoring_spsc_try_pop(ring, &out), 1);
    EXPECT_EQ(out.sequence_id, 42U);
    EXPECT_FLOAT_EQ(out.energy_mev, 12.5f);
    chronoring_spsc_destroy(ring);
}

TEST(CApiTest, SinkAndReaderWithCompression) {
    const std::filesystem::path path = temp_crng_path("sink");
    const std::string pstr = path.string();

    chronoring_sink_t* sink = chronoring_sink_create();
    ASSERT_NE(sink, nullptr);
    ASSERT_EQ(chronoring_sink_open(sink, pstr.c_str(), 4, 0x1Fu, 1), 1);

    std::vector<chronoring_event_frame_t> batch(1500);
    for (size_t i = 0; i < batch.size(); ++i) {
        batch[i] = make_c_event(i, (i % 2 == 0) ? 20.0f : 1.0f);
    }
    EXPECT_EQ(chronoring_sink_write_batch(sink, batch.data(), batch.size()), batch.size());
    EXPECT_EQ(chronoring_sink_flush(sink), 1);
    EXPECT_EQ(chronoring_sink_events_written(sink), batch.size());
    EXPECT_EQ(chronoring_sink_pages_written(sink), 2U);
    EXPECT_EQ(chronoring_sink_last_status(sink), 0);
    chronoring_sink_close(sink);
    chronoring_sink_destroy(sink);

    chronoring_reader_t* reader = chronoring_reader_open(pstr.c_str());
    ASSERT_NE(reader, nullptr);
    EXPECT_EQ(chronoring_reader_num_pages(reader), 2U);
    EXPECT_EQ(chronoring_reader_total_events(reader), batch.size());
    EXPECT_EQ(chronoring_reader_verify_all(reader, nullptr), 1);

    chronoring_trigger_config_t cfg = chronoring_default_trigger_config();
    cfg.min_energy_mev = 10.0f;
    uint64_t scanned = 0;
    const int64_t passed = chronoring_reader_replay_filtered(reader, &cfg, &scanned);
    EXPECT_EQ(scanned, batch.size());
    EXPECT_EQ(passed, 750);
    chronoring_reader_destroy(reader);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(CApiTest, ShmRingCreateOpenAndUnlink) {
    const auto now = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const std::string name = "cr_capi_" + std::to_string(now % 1000000ULL);
    (void)chronoring_shm_unlink(name.c_str());

    chronoring_shm_t* prod = chronoring_shm_create(name.c_str(), 64, 0);
    ASSERT_NE(prod, nullptr);
    chronoring_shm_t* cons = chronoring_shm_open(name.c_str());
    ASSERT_NE(cons, nullptr);

    const chronoring_event_frame_t ev = make_c_event(7, 15.0f);
    EXPECT_EQ(chronoring_shm_try_push(prod, &ev), 1);
    EXPECT_EQ(chronoring_shm_size(cons), 1U);

    chronoring_event_frame_t out{};
    EXPECT_EQ(chronoring_shm_try_pop(cons, &out), 1);
    EXPECT_EQ(out.sequence_id, 7U);

    chronoring_shm_close(prod);
    EXPECT_EQ(chronoring_shm_is_closed(cons), 1);
    chronoring_shm_destroy(cons);
    chronoring_shm_destroy(prod);
    (void)chronoring_shm_unlink(name.c_str());
}

TEST(CApiTest, PipelineLifecycleAndStats) {
    const std::filesystem::path path = temp_crng_path("pipe");
    const std::string pstr = path.string();

    chronoring_pipeline_config_t cfg = chronoring_default_pipeline_config();
    cfg.output_path = pstr.c_str();
    cfg.trigger.min_energy_mev = 10.0f;
    cfg.enable_compression = 1;

    chronoring_pipeline_t* pipe = chronoring_pipeline_create(&cfg);
    ASSERT_NE(pipe, nullptr);
    ASSERT_EQ(chronoring_pipeline_start(pipe), 1);

    for (size_t i = 0; i < 400; ++i) {
        const chronoring_event_frame_t ev = make_c_event(i, (i % 2 == 0) ? 20.0f : 1.0f);
        (void)chronoring_pipeline_submit(pipe, &ev);
    }
    chronoring_pipeline_stop(pipe);

    chronoring_pipeline_stats_t st{};
    chronoring_pipeline_stats(pipe, &st);
    EXPECT_EQ(st.last_status, 0);
    EXPECT_EQ(st.sink_errors, 0U);
    EXPECT_EQ(st.consumed_events, st.passed_events + st.rejected_events);
    chronoring_pipeline_destroy(pipe);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}
