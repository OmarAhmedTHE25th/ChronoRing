#pragma once

#include "chronoring/crypto_integrity.hpp"
#include "chronoring/event_frame.hpp"
#include "chronoring/status.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace chronoring {

inline constexpr std::uint32_t kWalMagic = 0x4357414Cu; // "CWAL"

struct DurableStoreConfig {
    std::string primary_dir{};
    std::vector<std::string> sync_replica_dirs{};
    std::size_t sync_quorum{1};
    std::size_t group_commit_max_pages{4};
    bool enable_compression{false};
    bool enable_pq_signatures{false};
    std::uint64_t pq_seed{0xC4090516ULL};
};

struct CommitRecord {
    std::uint64_t lsn{0};
    std::uint64_t commit_timestamp_ns{0};
    std::uint64_t min_event_ts{0};
    std::uint64_t max_event_ts{0};
    std::uint32_t event_count{0};
    Sha256Digest page_digest{};
    Sha256Digest merkle_root{};
    bool has_pq_signature{false};
    PqSignature pq_sig{};
};

// ACID-like durable WAL columnar store supporting:
//  - Group commit (`group_commit_max_pages` + explicit `flush_group_commit()`)
//  - Synchronous replication with configurable `sync_quorum`
//  - Per-page SHA-256 + running PageMerkleTree + optional WOTS+ PQ signatures
//  - Crash recovery (truncating torn/corrupt WAL tails)
//  - Time-travel / point-in-time recovery (`recover_to_lsn`, `recover_to_timestamp`)
class DurableColumnarStore {
public:
    DurableColumnarStore() = default;
    ~DurableColumnarStore();

    DurableColumnarStore(const DurableColumnarStore&) = delete;
    DurableColumnarStore& operator=(const DurableColumnarStore&) = delete;

    [[nodiscard]] RingStatus open(DurableStoreConfig config);
    void close() noexcept;

    [[nodiscard]] bool is_open() const noexcept { return is_open_; }

    // Stage a finalized ColumnarPage into the active group-commit batch.
    // Automatically calls `flush_group_commit()` once `group_commit_max_pages` is buffered.
    [[nodiscard]] Expected<std::uint64_t, RingStatus> append_page(
        const ColumnarPage& page,
        std::uint64_t commit_timestamp_ns = 0);

    // Flush and fsync all staged pages to primary + synchronous replicas.
    [[nodiscard]] RingStatus flush_group_commit();

    // Scan WAL, verify CRCs/SHA-256/PQ signatures, truncate torn tail on crash, rebuild index.
    [[nodiscard]] Expected<std::size_t, RingStatus> recover();

    // Point-in-Time Recovery (PITR) exporting matching pages into a `.crng` columnar file.
    [[nodiscard]] Expected<std::size_t, RingStatus> recover_to_lsn(
        std::uint64_t target_lsn, const std::string& out_crng_path) const;

    [[nodiscard]] Expected<std::size_t, RingStatus> recover_to_timestamp(
        std::uint64_t target_commit_ts_ns, const std::string& out_crng_path) const;

    [[nodiscard]] std::size_t committed_pages() const noexcept { return history_.size(); }
    [[nodiscard]] std::size_t pending_pages() const noexcept { return pending_.size(); }
    [[nodiscard]] std::uint64_t last_lsn() const noexcept;
    [[nodiscard]] Sha256Digest current_merkle_root() const { return merkle_.root(); }
    [[nodiscard]] const PageMerkleTree& merkle_tree() const noexcept { return merkle_; }
    [[nodiscard]] const std::vector<CommitRecord>& commit_history() const noexcept { return history_; }
    [[nodiscard]] bool verify_commit_pq_signature(std::size_t commit_index) const noexcept;
    [[nodiscard]] const std::string& last_error() const noexcept { return last_error_; }

private:
    struct PendingEntry {
        CommitRecord record{};
        std::vector<std::uint8_t> encoded_payload{};
        std::uint16_t wal_flags{0};
    };

    struct WalIndexEntry {
        std::uint64_t file_offset{0};
        std::uint64_t payload_bytes{0};
        std::uint16_t flags{0};
    };

    DurableStoreConfig config_{};
    bool is_open_{false};
    std::string primary_wal_path_{};
    PageMerkleTree merkle_{};
    std::vector<CommitRecord> history_{};
    std::vector<WalIndexEntry> wal_index_{};
    std::vector<PendingEntry> pending_{};
    std::vector<std::uint8_t> compress_scratch_{};
    std::uint64_t next_lsn_{1};
    std::uint64_t wal_valid_bytes_{0};
    std::string last_error_{};
};

} // namespace chronoring
