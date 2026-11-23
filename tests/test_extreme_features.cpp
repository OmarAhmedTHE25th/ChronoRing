#include "chronoring/columnar_sink.hpp"
#include "chronoring/deterministic_replay.hpp"
#include "chronoring/event_crypto.hpp"
#include "chronoring/raft_cluster.hpp"
#include "chronoring/scientific_interop.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
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

TEST(ExtremeFeaturesTest, ChaCha20Poly1305Rfc8439KnownAnswerTest) {
    // RFC 8439 Section 2.8.2 AEAD_CHACHA20_POLY1305 test vector.
    const CryptoKey256 key{
        0x80U, 0x81U, 0x82U, 0x83U, 0x84U, 0x85U, 0x86U, 0x87U,
        0x88U, 0x89U, 0x8AU, 0x8BU, 0x8CU, 0x8DU, 0x8EU, 0x8FU,
        0x90U, 0x91U, 0x92U, 0x93U, 0x94U, 0x95U, 0x96U, 0x97U,
        0x98U, 0x99U, 0x9AU, 0x9BU, 0x9CU, 0x9DU, 0x9EU, 0x9FU,
    };
    const Nonce96 nonce{
        0x07U, 0x00U, 0x00U, 0x00U, 0x40U, 0x41U,
        0x42U, 0x43U, 0x44U, 0x45U, 0x46U, 0x47U,
    };
    const std::array<std::uint8_t, 12> aad{
        0x50U, 0x51U, 0x52U, 0x53U, 0xC0U, 0xC1U,
        0xC2U, 0xC3U, 0xC4U, 0xC5U, 0xC6U, 0xC7U,
    };
    const std::string plaintext =
        "Ladies and Gentlemen of the class of '99: If I could offer you only one "
        "tip for the future, sunscreen would be it.";
    ASSERT_EQ(plaintext.size(), 114U);

    const std::array<std::uint8_t, 114> expected_ct{
        0xD3U, 0x1AU, 0x8DU, 0x34U, 0x64U, 0x8EU, 0x60U, 0xDBU,
        0x7BU, 0x86U, 0xAFU, 0xBCU, 0x53U, 0xEFU, 0x7EU, 0xC2U,
        0xA4U, 0xADU, 0xEDU, 0x51U, 0x29U, 0x6EU, 0x08U, 0xFEU,
        0xA9U, 0xE2U, 0xB5U, 0xA7U, 0x36U, 0xEEU, 0x62U, 0xD6U,
        0x3DU, 0xBEU, 0xA4U, 0x5EU, 0x8CU, 0xA9U, 0x67U, 0x12U,
        0x82U, 0xFAU, 0xFBU, 0x69U, 0xDAU, 0x92U, 0x72U, 0x8BU,
        0x1AU, 0x71U, 0xDEU, 0x0AU, 0x9EU, 0x06U, 0x0BU, 0x29U,
        0x05U, 0xD6U, 0xA5U, 0xB6U, 0x7EU, 0xCDU, 0x3BU, 0x36U,
        0x92U, 0xDDU, 0xBDU, 0x7FU, 0x2DU, 0x77U, 0x8BU, 0x8CU,
        0x98U, 0x03U, 0xAEU, 0xE3U, 0x28U, 0x09U, 0x1BU, 0x58U,
        0xFAU, 0xB3U, 0x24U, 0xE4U, 0xFAU, 0xD6U, 0x75U, 0x94U,
        0x55U, 0x85U, 0x80U, 0x8BU, 0x48U, 0x31U, 0xD7U, 0xBCU,
        0x3FU, 0xF4U, 0xDEU, 0xF0U, 0x8EU, 0x4BU, 0x7AU, 0x9DU,
        0xE5U, 0x76U, 0xD2U, 0x65U, 0x86U, 0xCEU, 0xC6U, 0x4BU,
        0x61U, 0x16U,
    };
    const AuthTag128 expected_tag{
        0x1AU, 0xE1U, 0x0BU, 0x59U, 0x4FU, 0x09U, 0xE2U, 0x6AU,
        0x7EU, 0x90U, 0x2EU, 0xCBU, 0xD0U, 0x60U, 0x06U, 0x91U,
    };

    std::array<std::uint8_t, 114> ct{};
    AuthTag128 tag{};
    chacha20_poly1305_encrypt(
        key,
        nonce,
        aad,
        std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(plaintext.data()), plaintext.size()),
        ct.data(),
        tag);
    EXPECT_EQ(ct, expected_ct);
    EXPECT_EQ(tag, expected_tag);

    std::array<std::uint8_t, 114> pt_out{};
    EXPECT_TRUE(chacha20_poly1305_decrypt(key, nonce, aad, ct, tag, pt_out.data()));
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(pt_out.data()), pt_out.size()), plaintext);

    // Flipping a bit in AAD, ciphertext, or tag must fail authentication.
    auto bad_aad = aad;
    bad_aad[0] ^= 0x01U;
    EXPECT_FALSE(chacha20_poly1305_decrypt(key, nonce, bad_aad, ct, tag, pt_out.data()));

    auto bad_tag = tag;
    bad_tag[15] ^= 0x80U;
    EXPECT_FALSE(chacha20_poly1305_decrypt(key, nonce, aad, ct, bad_tag, pt_out.data()));
}

TEST(ExtremeFeaturesTest, ChaCha20Poly1305SubkeySeparationNoncesAndTrapdoors) {
    DetectorKeyVault vault;
    CryptoKey256 master{};
    for (std::size_t i = 0; i < master.size(); ++i) {
        master[i] = static_cast<std::uint8_t>(i * 11U + 3U);
    }
    vault.derive_detector_key(master, 0);
    vault.derive_detector_key(master, 1);

    const CryptoKey256 root0 = *vault.get_key(0);
    const CryptoKey256 root1 = *vault.get_key(1);
    const CryptoKey256 aead0 = *vault.get_aead_key(0);
    const CryptoKey256 trap0 = *vault.get_trapdoor_key(0);
    EXPECT_NE(root0, root1);
    EXPECT_NE(aead0, root0);
    EXPECT_NE(trap0, root0);
    EXPECT_NE(aead0, trap0);

    const auto trapdoor = make_encrypted_flag_trapdoor(0, vault);
    ASSERT_TRUE(trapdoor.has_value());

    DetectorEncryptor encryptor(vault, 0x11223344U);

    // Encrypting the EXACT SAME (detector_id, sequence_id) frame twice MUST yield
    // distinct nonces and distinct ciphertexts (stateful counter prevents nonce reuse).
    const EventFrame dup_ev =
        make_test_ev(42, 0, 50.0f, -30.0f, 0.1f, kFlagValid | kFlagCalibrated);
    const auto enc_dup1 = encryptor.encrypt(dup_ev);
    const auto enc_dup2 = encryptor.encrypt(dup_ev);
    ASSERT_TRUE(enc_dup1.has_value());
    ASSERT_TRUE(enc_dup2.has_value());
    EXPECT_NE(enc_dup1->nonce, enc_dup2->nonce);
    EXPECT_NE(enc_dup1->ciphertext, enc_dup2->ciphertext);
    EXPECT_TRUE(decrypt_event_frame(*enc_dup1, vault).has_value());
    EXPECT_TRUE(decrypt_event_frame(*enc_dup2, vault).has_value());

    const std::vector<EventFrame> events = {
        make_test_ev(1, 0, 50.0f, -30.0f, 0.1f, kFlagValid | kFlagCalibrated), // healthy det 0
        make_test_ev(2, 0, 80.0f, 20.0f, 0.1f, kFlagValid | kFlagVeto),        // vetoed det 0
        make_test_ev(3, 1, 80.0f, 20.0f, 0.1f, kFlagValid | kFlagCalibrated),  // healthy det 1
    };

    for (std::size_t i = 0; i < events.size(); ++i) {
        const auto enc = encryptor.encrypt(events[i]);
        ASSERT_TRUE(enc.has_value());

        const auto dec = decrypt_event_frame(*enc, vault);
        ASSERT_TRUE(dec.has_value());
        EXPECT_EQ(dec->sequence_id, events[i].sequence_id);
        EXPECT_FLOAT_EQ(dec->energy_mev, events[i].energy_mev);

        // Ciphertext 1-bit tamper must be rejected by AEAD Poly1305.
        EncryptedEventFrame tampered = *enc;
        tampered.ciphertext[16] ^= 0x01U;
        EXPECT_FALSE(decrypt_event_frame(tampered, vault).has_value());

        // Flag trapdoor is authenticated in AAD; swapping/tampering it must fail decryption.
        EncryptedEventFrame swapped_td = *enc;
        swapped_td.flag_trapdoor[0] ^= 0x01U;
        EXPECT_FALSE(decrypt_event_frame(swapped_td, vault).has_value());

        // HMAC-SHA256 flag trapdoor matches healthy detector-0 events only.
        EXPECT_EQ(evaluate_encrypted_flag_trapdoor(*enc, *trapdoor), i == 0U);
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
