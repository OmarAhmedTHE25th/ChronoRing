#include "chronoring/columnar_sink.hpp"
#include "chronoring/deterministic_replay.hpp"
#include "chronoring/event_crypto.hpp"
#include "chronoring/raft_cluster.hpp"
#include "chronoring/scientific_interop.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace chronoring::test {

namespace {

EventFrame make_test_ev(std::uint64_t seq,
                        std::uint32_t det,
                        float energy,
                        float timing,
                        float pileup,
                        std::uint32_t flags) noexcept {
    EventFrame f{};
    f.timestamp_ns = 10'000ULL + seq * 50ULL;
    f.sequence_id = seq;
    f.detector_id = det;
    f.channel_id = static_cast<std::uint32_t>(seq % 64U);
    f.energy_mev = energy;
    f.timing_ps = timing;
    f.pileup_score = pileup;
    f.quality_flags = flags;
    f.multiplicity = 2;
    return f;
}

} // namespace

TEST(ExtremeFeaturesTest, ChaCha20Poly1305AndEncryptedTriggering) {
    DetectorKeyVault vault;
    CryptoKey256 master{};
    for (std::size_t i = 0; i < master.size(); ++i) {
        master[i] = static_cast<std::uint8_t>(i * 11U + 3U);
    }
    vault.derive_detector_key(master, 0);
    vault.derive_detector_key(master, 1);

    const CryptoKey256 k0 = *vault.get_key(0);
    const CryptoKey256 k1 = *vault.get_key(1);
    EXPECT_NE(k0, k1);

    // Order-preserving float encoding monotonicity
    EXPECT_LT(ope_encode_f32(-50.0f, k0), ope_encode_f32(-0.5f, k0));
    EXPECT_LT(ope_encode_f32(-0.5f, k0), ope_encode_f32(0.0f, k0));
    EXPECT_LT(ope_encode_f32(0.0f, k0), ope_encode_f32(12.5f, k0));
    EXPECT_LT(ope_encode_f32(12.5f, k0), ope_encode_f32(12.501f, k0));

    TriggerConfig cfg{};
    cfg.min_energy_mev = 10.0f;
    cfg.max_energy_mev = 500.0f;
    cfg.max_abs_timing_ps = 120.0f;
    cfg.max_pileup_score = 0.4f;
    cfg.required_flags = kFlagValid | kFlagCalibrated;
    cfg.forbidden_flags = kFlagSaturated | kFlagVeto;

    const auto trapdoor = make_encrypted_trigger_trapdoor(0, cfg, vault);
    ASSERT_TRUE(trapdoor.has_value());

    const std::vector<EventFrame> events = {
        make_test_ev(1, 0, 50.0f, -30.0f, 0.1f, kFlagValid | kFlagCalibrated), // pass
        make_test_ev(2, 0, 5.0f, 10.0f, 0.1f, kFlagValid | kFlagCalibrated),   // low E
        make_test_ev(3, 0, 80.0f, 200.0f, 0.1f, kFlagValid | kFlagCalibrated), // timing
        make_test_ev(4, 0, 80.0f, 20.0f, 0.8f, kFlagValid | kFlagCalibrated),  // pileup
        make_test_ev(5, 0, 80.0f, 20.0f, 0.1f, kFlagValid | kFlagVeto),       // vetoed
        make_test_ev(6, 1, 80.0f, 20.0f, 0.1f, kFlagValid | kFlagCalibrated)  // wrong detector key
    };

    for (std::size_t i = 0; i < events.size(); ++i) {
        const auto enc = encrypt_event_frame(events[i], vault);
        ASSERT_TRUE(enc.has_value());

        const auto dec = decrypt_event_frame(*enc, vault);
        ASSERT_TRUE(dec.has_value());
        EXPECT_EQ(dec->sequence_id, events[i].sequence_id);
        EXPECT_FLOAT_EQ(dec->energy_mev, events[i].energy_mev);

        // Ciphertext 1-bit tamper must be rejected by AEAD Poly1305
        EncryptedEventFrame tampered = *enc;
        tampered.ciphertext[16] ^= 0x01U;
        EXPECT_FALSE(decrypt_event_frame(tampered, vault).has_value());

        // Searchable encrypted trigger matches plaintext evaluation only for detector 0
        const bool enc_hit = evaluate_encrypted_trigger(*enc, *trapdoor);
        EXPECT_EQ(enc_hit, i == 0U);
    }
}

TEST(ExtremeFeaturesTest, DeterministicReplayBitIdenticalTranscript) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path ep1 =
        std::filesystem::temp_directory_path() /
        ("cr_rep1_" + std::to_string(now) + ".crng");
    const std::filesystem::path ep2 =
        std::filesystem::temp_directory_path() /
        ("cr_rep2_" + std::to_string(now) + ".crng");

    {
        ColumnarSink s1(ep1.string(), 4, kColAll, false);
        ColumnarSink s2(ep2.string(), 4, kColAll, true);
        ASSERT_TRUE(s1.is_open());
        ASSERT_TRUE(s2.is_open());
        for (std::uint64_t i = 0; i < 600; ++i) {
            const float e = (i % 3 == 0) ? 2.0f : 40.0f;
            const EventFrame ev =
                make_test_ev(i, 0, e, 25.0f, 0.1f, kFlagValid | kFlagCalibrated);
            EXPECT_TRUE(s1.write(ev));
            EXPECT_TRUE(s2.write(make_test_ev(600ULL + i, 0, e, 25.0f, 0.1f, kFlagValid | kFlagCalibrated)));
        }
        s1.close();
        s2.close();
    }

    DeterministicReplayEngine replay;
    const TriggerConfig cfg{};
    const auto r_scalar = replay.replay_files({ep1.string(), ep2.string()}, cfg, SimdBackend::Scalar);
    const auto r_auto = replay.replay_files({ep1.string(), ep2.string()}, cfg, SimdBackend::Auto);

    ASSERT_TRUE(r_scalar.has_value());
    ASSERT_TRUE(r_auto.has_value());
    EXPECT_EQ(r_scalar->files_processed, 2U);
    EXPECT_EQ(r_scalar->total_events, 1200U);
    EXPECT_EQ(r_scalar->passed_events, 1200U);
    EXPECT_TRUE(constant_time_equal(r_scalar->output_digest, r_auto->output_digest));

    std::error_code ec;
    std::filesystem::remove(ep1, ec);
    std::filesystem::remove(ep2, ec);
}

TEST(ExtremeFeaturesTest, RaftClusterElectionReplicationAndFailover) {
    RaftRingCluster cluster(5);
    ASSERT_EQ(cluster.elect_leader(0), RingStatus::Ok);
    EXPECT_EQ(cluster.leader_id(), 0);
    EXPECT_EQ(cluster.quorum_size(), 3U);

    for (std::uint64_t i = 0; i < 50; ++i) {
        ASSERT_EQ(
            cluster.replicate_frame(
                make_test_ev(i, 0, 20.0f, 10.0f, 0.1f, kFlagValid | kFlagCalibrated)),
            RingStatus::Ok);
    }

    // Isolate 2 followers (3/5 online -> quorum intact)
    cluster.set_node_online(3, false);
    cluster.set_node_online(4, false);
    for (std::uint64_t i = 50; i < 75; ++i) {
        ASSERT_EQ(
            cluster.replicate_frame(
                make_test_ev(i, 0, 20.0f, 10.0f, 0.1f, kFlagValid | kFlagCalibrated)),
            RingStatus::Ok);
    }

    // Leader fails; bring node 3 back and elect node 1 in term 2
    cluster.set_node_online(0, false);
    cluster.set_node_online(3, true);
    ASSERT_EQ(cluster.elect_leader(1), RingStatus::Ok);
    EXPECT_EQ(cluster.leader_id(), 1);
    EXPECT_EQ(cluster.node(1).current_term(), 2U);

    const auto committed = cluster.node(1).committed_frames();
    ASSERT_EQ(committed.size(), 75U);
    EXPECT_EQ(committed.back().sequence_id, 74U);

    // Take 2 more nodes offline (only 2/5 online < quorum 3) -> replication must refuse
    cluster.set_node_online(2, false);
    cluster.set_node_online(3, false);
    EXPECT_EQ(
        cluster.replicate_frame(make_test_ev(75, 0, 20.0f, 10.0f, 0.1f, kFlagValid)),
        RingStatus::IoError);
}

TEST(ExtremeFeaturesTest, ZeroCopyArrowCDataAndHdf5RootSchema) {
    ColumnarPage page{};
    page.header.magic = kColumnarPageMagic;
    page.header.version = kColumnarFormatVersion;
    page.header.event_count = 16;
    page.header.min_timestamp_ns = 1000;
    page.header.max_timestamp_ns = 2500;
    page.timestamp_ns[0] = 1000;
    page.energy_mev[0] = 42.5f;

    ChronoArrowSchema schema{};
    ChronoArrowArray array{};
    ASSERT_EQ(export_columnar_page_to_arrow(page, schema, array), RingStatus::Ok);
    EXPECT_STREQ(schema.format, "+s");
    EXPECT_EQ(schema.n_children, 13);
    EXPECT_EQ(array.length, 16);
    EXPECT_EQ(array.n_children, 13);
    // Strict zero-copy buffer pointer identity verification
    EXPECT_EQ(array.children[0]->buffers[1], static_cast<const void*>(page.timestamp_ns.data()));
    EXPECT_EQ(array.children[4]->buffers[1], static_cast<const void*>(page.energy_mev.data()));
    EXPECT_EQ(array.children[12]->buffers[1], static_cast<const void*>(page.trigger_mask.data()));

    schema.release(&schema);
    array.release(&array);
    EXPECT_EQ(schema.release, nullptr);
    EXPECT_EQ(array.release, nullptr);

    const std::string meta = export_hdf5_root_schema_json(page);
    EXPECT_NE(meta.find("\"parquet_arrow_root_compatible\": true"), std::string::npos);
    EXPECT_NE(meta.find("ULong64_t"), std::string::npos);
}

} // namespace chronoring::test
