#include "chronoring/columnar_sink.hpp"
#include "chronoring/crypto_integrity.hpp"
#include "chronoring/durable_store.hpp"
#include "chronoring/event_frame.hpp"
#include "chronoring/tiered_storage.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

namespace chronoring::test {

namespace {

std::filesystem::path make_temp_dir(const char* prefix) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        (std::string("chronoring_dur_") + prefix + "_" + std::to_string(now));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

ColumnarPage make_populated_page(std::uint64_t base_seq, std::size_t count) {
    ColumnarPage page{};
    std::vector<EventFrame> events(count);
    for (std::size_t i = 0; i < count; ++i) {
        events[i].timestamp_ns = 10'000ULL + (base_seq + i) * 100ULL;
        events[i].sequence_id = base_seq + i;
        events[i].detector_id = static_cast<std::uint32_t>(i % 4U);
        events[i].channel_id = static_cast<std::uint32_t>(i % 64U);
        events[i].energy_mev = 15.0f + static_cast<float>(i % 20U);
        events[i].timing_ps = 20.0f;
        events[i].pileup_score = 0.05f;
        events[i].quality_flags = kFlagValid | kFlagCalibrated;
        events[i].multiplicity = 2;
    }
    transpose_aos_to_soa(events.data(), events.size(), page, kColAll);
    finalize_page_crc(page);
    return page;
}

} // namespace

TEST(DurableStorageTest, AcidGroupCommitWalCrashRecoveryAndPitr) {
    const std::filesystem::path root = make_temp_dir("acid");
    const std::filesystem::path primary_dir = root / "primary";
    const std::filesystem::path replica_dir = root / "replica";

    DurableStoreConfig cfg;
    cfg.primary_dir = primary_dir.string();
    cfg.sync_replica_dirs = {replica_dir.string()};
    cfg.sync_quorum = 2;
    cfg.group_commit_max_pages = 3;
    cfg.enable_compression = true;
    cfg.enable_pq_signatures = true;
    cfg.pq_seed = 0x123456789ABCDEF0ULL;

    std::vector<Sha256Digest> recorded_leaves;
    Sha256Digest final_root{};

    {
        DurableColumnarStore store;
        ASSERT_EQ(store.open(cfg), RingStatus::Ok) << store.last_error();

        for (std::uint64_t i = 0; i < 5; ++i) {
            const ColumnarPage page = make_populated_page(i * 1024ULL, 1024U);
            const auto lsn = store.append_page(page, (i + 1ULL) * 1000ULL);
            ASSERT_TRUE(lsn.has_value());
            EXPECT_EQ(*lsn, i + 1ULL);
            if (i < 2) {
                EXPECT_EQ(store.committed_pages(), 0U);
                EXPECT_EQ(store.pending_pages(), i + 1ULL);
            } else if (i == 2) {
                // Auto group commit triggered at 3 pages
                EXPECT_EQ(store.committed_pages(), 3U);
                EXPECT_EQ(store.pending_pages(), 0U);
            }
        }
        ASSERT_EQ(store.flush_group_commit(), RingStatus::Ok);
        EXPECT_EQ(store.committed_pages(), 5U);
        EXPECT_EQ(store.last_lsn(), 5U);

        // Synchronous replica WAL must match primary WAL byte-for-byte
        ASSERT_TRUE(std::filesystem::exists(primary_dir / "wal.log"));
        ASSERT_TRUE(std::filesystem::exists(replica_dir / "wal.log"));
        EXPECT_EQ(std::filesystem::file_size(primary_dir / "wal.log"),
                  std::filesystem::file_size(replica_dir / "wal.log"));

        final_root = store.current_merkle_root();
        for (std::size_t i = 0; i < 5; ++i) {
            EXPECT_TRUE(store.verify_commit_pq_signature(i));
            const Sha256Digest leaf =
                PageMerkleTree::hash_leaf(store.commit_history()[i].page_digest.data(),
                                          store.commit_history()[i].page_digest.size());
            recorded_leaves.push_back(leaf);
            const auto proof = store.merkle_tree().generate_proof(i);
            ASSERT_TRUE(proof.has_value());
            EXPECT_TRUE(PageMerkleTree::verify_proof(leaf, *proof, final_root));
        }
        store.close();
    }

    // Simulate power failure crash leaving a torn/garbage tail at the end of primary WAL
    {
        std::ofstream ofs(primary_dir / "wal.log", std::ios::binary | std::ios::app);
        ASSERT_TRUE(ofs.is_open());
        const char torn_bytes[64] = {'T', 'O', 'R', 'N'};
        ofs.write(torn_bytes, sizeof(torn_bytes));
    }

    // Crash recovery + Point-in-Time Recovery (PITR)
    {
        DurableColumnarStore recovered;
        ASSERT_EQ(recovered.open(cfg), RingStatus::Ok) << recovered.last_error();
        EXPECT_EQ(recovered.committed_pages(), 5U);
        EXPECT_TRUE(constant_time_equal(recovered.current_merkle_root(), final_root));
        for (std::size_t i = 0; i < 5; ++i) {
            EXPECT_TRUE(recovered.verify_commit_pq_signature(i));
        }

        const std::filesystem::path pitr_lsn_path = root / "pitr_lsn3.crng";
        const auto pitr3 = recovered.recover_to_lsn(3, pitr_lsn_path.string());
        ASSERT_TRUE(pitr3.has_value());
        EXPECT_EQ(*pitr3, 3U);

        ColumnarReader reader3(pitr_lsn_path.string());
        ASSERT_TRUE(reader3.is_open());
        EXPECT_EQ(reader3.num_pages(), 3U);
        EXPECT_EQ(reader3.total_events(), 3U * 1024U);
        EXPECT_TRUE(reader3.verify_all_pages());
        reader3.close();

        const std::filesystem::path pitr_ts_path = root / "pitr_ts4000.crng";
        const auto pitr4 = recovered.recover_to_timestamp(4000ULL, pitr_ts_path.string());
        ASSERT_TRUE(pitr4.has_value());
        EXPECT_EQ(*pitr4, 4U);

        ColumnarReader reader4(pitr_ts_path.string());
        ASSERT_TRUE(reader4.is_open());
        EXPECT_EQ(reader4.num_pages(), 4U);
        EXPECT_TRUE(reader4.verify_all_pages());
        const auto replay = reader4.replay_filtered(TriggerConfig{});
        ASSERT_TRUE(replay.has_value());
        EXPECT_EQ(*replay, 4U * 1024U);
        reader4.close();
        recovered.close();
    }

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST(DurableStorageTest, MultiTierAutomaticMigrationAndReadThrough) {
    const std::filesystem::path root = make_temp_dir("tiers");
    LocalObjectStore cold_s3((root / "s3_cold").string(), "ceph-cold");

    TierPolicy policy;
    policy.hot_max_pages = 2;
    policy.warm_max_pages = 3;

    TieredStorageManager mgr(
        (root / "nvme_hot").string(),
        (root / "ssd_warm").string(),
        &cold_s3,
        policy);

    std::vector<std::vector<std::uint8_t>> expected_pages(10);
    for (std::uint64_t id = 0; id < 10; ++id) {
        const ColumnarPage page = make_populated_page(id * 100ULL, 128U);
        expected_pages[id].resize(sizeof(ColumnarPage));
        std::memcpy(expected_pages[id].data(), static_cast<const void*>(&page), sizeof(ColumnarPage));
        ASSERT_EQ(mgr.put_page(id, expected_pages[id]), RingStatus::Ok);
    }

    EXPECT_EQ(mgr.count_in_tier(StorageTier::HotNvme), 2U);   // pages 8, 9
    EXPECT_EQ(mgr.count_in_tier(StorageTier::WarmSsd), 3U);   // pages 5, 6, 7
    EXPECT_EQ(mgr.count_in_tier(StorageTier::ColdObject), 5U); // pages 0..4

    EXPECT_EQ(*mgr.locate_page(0), StorageTier::ColdObject);
    EXPECT_EQ(*mgr.locate_page(6), StorageTier::WarmSsd);
    EXPECT_EQ(*mgr.locate_page(9), StorageTier::HotNvme);

    for (std::uint64_t id = 0; id < 10; ++id) {
        const auto fetched = mgr.get_page(id);
        ASSERT_TRUE(fetched.has_value()) << "Missing page " << id;
        EXPECT_EQ(*fetched, expected_pages[id]);
    }

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST(DurableStorageTest, GeoReplicatedErasureCodedBackupSurvivesRegionOutages) {
    const std::filesystem::path root = make_temp_dir("geo");

    std::vector<LocalObjectStore> owned_regions;
    owned_regions.reserve(6);
    for (int r = 0; r < 6; ++r) {
        owned_regions.emplace_back(
            (root / ("region_" + std::to_string(r))).string(),
            "region-" + std::to_string(r));
    }
    std::vector<ObjectStoreClient*> ptrs;
    for (auto& reg : owned_regions) {
        ptrs.push_back(&reg);
    }

    GeoReplicatedBackup geo(4, 2, ptrs);
    ASSERT_TRUE(geo.is_valid());

    const ColumnarPage page = make_populated_page(4200ULL, 512U);
    std::vector<std::uint8_t> payload(sizeof(ColumnarPage));
    std::memcpy(payload.data(), static_cast<const void*>(&page), sizeof(ColumnarPage));
    const Sha256Digest root_hash = PageMerkleTree::hash_leaf(payload.data(), payload.size());

    ASSERT_EQ(geo.backup_page(77, payload, root_hash), RingStatus::Ok);

    // Take 2 regions completely offline -> still recoverable (K=4 surviving out of K+M=6)
    owned_regions[1].set_offline(true);
    owned_regions[4].set_offline(true);
    const auto restored = geo.restore_page(77);
    ASSERT_TRUE(restored.has_value());
    EXPECT_EQ(*restored, payload);

    // Take a 3rd region offline -> 3 < K=4 -> reconstruction must fail cleanly
    owned_regions[0].set_offline(true);
    const auto failed = geo.restore_page(77);
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error(), RingStatus::CorruptedPage);

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

} // namespace chronoring::test
