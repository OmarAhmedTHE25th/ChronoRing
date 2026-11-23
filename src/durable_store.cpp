#include "chronoring/durable_store.hpp"
#include "chronoring/columnar_sink.hpp"
#include "chronoring/lz4_codec.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace chronoring {

namespace {

PqSeed256 derive_lsn_pq_seed(const PqSeed256& master_seed, std::uint64_t lsn) noexcept {
    static constexpr std::uint8_t kSalt[] = "chronoring/wal/wots+/v2";
    std::array<std::uint8_t, 12> info{'l', 's', 'n', ':', 0, 0, 0, 0, 0, 0, 0, 0};
    for (std::size_t b = 0; b < 8; ++b) {
        info[4 + b] = static_cast<std::uint8_t>((lsn >> (b * 8)) & 0xFFU);
    }
    return hkdf_sha256(
        std::span<const std::uint8_t>(kSalt, sizeof(kSalt) - 1),
        master_seed,
        info);
}

struct WalRecordHeader {
    std::uint32_t magic{kWalMagic};
    std::uint16_t version{1u};
    std::uint16_t flags{0u};
    std::uint64_t lsn{0};
    std::uint64_t commit_timestamp_ns{0};
    std::uint64_t min_event_ts{0};
    std::uint64_t max_event_ts{0};
    std::uint32_t event_count{0};
    std::uint32_t header_crc32c{0};
    std::uint64_t payload_bytes{0};
    std::uint32_t payload_crc32c{0};
    std::uint8_t has_pq_signature{0};
    std::uint8_t pad[3]{};
    Sha256Digest page_digest{};
    Sha256Digest merkle_root{};
};

static_assert(sizeof(WalRecordHeader) == 128, "WalRecordHeader must occupy 2 cache lines");
static_assert(std::is_trivially_copyable_v<WalRecordHeader>);

std::uint64_t steady_now_ns() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

bool append_and_fsync_file(const std::string& path,
                           const std::vector<std::uint8_t>& bytes) noexcept {
    if (bytes.empty()) {
        return true;
    }
#ifdef _WIN32
    HANDLE fh = CreateFileA(path.c_str(), FILE_APPEND_DATA,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fh == INVALID_HANDLE_VALUE) {
        return false;
    }
    std::size_t off = 0;
    bool ok = true;
    while (off < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - off, 1U << 20));
        DWORD written = 0;
        if (!WriteFile(fh, bytes.data() + off, chunk, &written, nullptr) || written == 0) {
            ok = false;
            break;
        }
        off += written;
    }
    if (ok && !FlushFileBuffers(fh)) {
        ok = false;
    }
    CloseHandle(fh);
    return ok;
#else
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        return false;
    }
    std::size_t off = 0;
    bool ok = true;
    while (off < bytes.size()) {
        const ssize_t n = ::write(fd, bytes.data() + off, bytes.size() - off);
        if (n <= 0) {
            ok = false;
            break;
        }
        off += static_cast<std::size_t>(n);
    }
    if (ok && ::fsync(fd) != 0) {
        ok = false;
    }
    ::close(fd);
    return ok;
#endif
}

std::vector<std::uint8_t> encode_wal_payload(const ColumnarPage& page,
                                             bool enable_compression,
                                             std::vector<std::uint8_t>& scratch) {
    ColumnarPage copy = page;
    copy.header.magic = kColumnarPageMagic;
    copy.header.version = kColumnarFormatVersion;
    copy.header.capacity = static_cast<std::uint32_t>(kColumnarPageEvents);
    copy.header.compressed_bytes = 0;

    constexpr std::size_t kBodyBytes = sizeof(ColumnarPage) - sizeof(ColumnarPageHeader);
    if (enable_compression) {
        if (scratch.size() < lz4_compress_bound(kBodyBytes)) {
            scratch.resize(lz4_compress_bound(kBodyBytes));
        }
        const auto* body =
            reinterpret_cast<const std::uint8_t*>(&copy) + sizeof(ColumnarPageHeader);
        const std::size_t comp_len =
            lz4_compress(body, kBodyBytes, scratch.data(), scratch.size());
        if (comp_len > 0 && comp_len < kBodyBytes) {
            copy.header.flags = static_cast<std::uint16_t>(copy.header.flags | kPageFlagLz4);
            copy.header.compressed_bytes = static_cast<std::uint32_t>(comp_len);
            copy.header.data_crc32c = crc32c(scratch.data(), comp_len);
            copy.header.header_crc32c = 0;
            copy.header.header_crc32c = crc32c(&copy.header, sizeof(ColumnarPageHeader));

            std::vector<std::uint8_t> out(sizeof(ColumnarPageHeader) + comp_len);
            std::memcpy(out.data(), &copy.header, sizeof(ColumnarPageHeader));
            std::memcpy(out.data() + sizeof(ColumnarPageHeader), scratch.data(), comp_len);
            return out;
        }
    }

    finalize_page_crc(copy);
    std::vector<std::uint8_t> out(sizeof(ColumnarPage));
    std::memcpy(out.data(), static_cast<const void*>(&copy), sizeof(ColumnarPage));
    return out;
}

} // namespace

DurableColumnarStore::~DurableColumnarStore() {
    close();
}

RingStatus DurableColumnarStore::open(DurableStoreConfig config) {
    close();
    if (config.primary_dir.empty() || config.group_commit_max_pages == 0 ||
        config.sync_quorum == 0) {
        last_error_ = "Invalid DurableStoreConfig (empty primary_dir or zero quorum/batch)";
        return RingStatus::InvalidArgument;
    }

    std::error_code ec;
    std::filesystem::create_directories(config.primary_dir, ec);
    if (ec) {
        last_error_ = "Failed to create primary_dir: " + ec.message();
        return RingStatus::IoError;
    }
    for (const std::string& rep : config.sync_replica_dirs) {
        if (!rep.empty()) {
            std::filesystem::create_directories(rep, ec);
        }
    }

    config_ = std::move(config);
    primary_wal_path_ =
        (std::filesystem::path(config_.primary_dir) / "wal.log").string();
    merkle_.clear();
    history_.clear();
    wal_index_.clear();
    pending_.clear();
    next_lsn_ = 1;
    wal_valid_bytes_ = 0;
    last_error_.clear();
    is_open_ = true;

    const auto rec_res = recover();
    if (!rec_res.has_value()) {
        close();
        return rec_res.error();
    }
    return RingStatus::Ok;
}

void DurableColumnarStore::close() noexcept {
    if (is_open_ && !pending_.empty()) {
        (void)flush_group_commit();
    }
    is_open_ = false;
    pending_.clear();
}

std::uint64_t DurableColumnarStore::last_lsn() const noexcept {
    return history_.empty() ? 0ULL : history_.back().lsn;
}

Expected<std::uint64_t, RingStatus> DurableColumnarStore::append_page(
    const ColumnarPage& page,
    std::uint64_t commit_timestamp_ns) {
    if (!is_open_) {
        return unexpected(RingStatus::Closed);
    }

    const std::uint64_t lsn = next_lsn_ + pending_.size();
    const std::uint64_t ts =
        (commit_timestamp_ns != 0) ? commit_timestamp_ns : steady_now_ns();

    std::vector<std::uint8_t> encoded =
        encode_wal_payload(page, config_.enable_compression, compress_scratch_);

    const Sha256Digest raw_digest = sha256(encoded.data(), encoded.size());

    // Build running Merkle root across committed + currently pending leaves.
    PageMerkleTree tmp_tree = merkle_;
    for (const auto& p : pending_) {
        (void)tmp_tree.append_leaf_digest(p.record.page_digest);
    }
    (void)tmp_tree.append_leaf_digest(raw_digest);
    const Sha256Digest root = tmp_tree.root();

    PendingEntry entry;
    entry.record.lsn = lsn;
    entry.record.commit_timestamp_ns = ts;
    entry.record.min_event_ts = page.header.min_timestamp_ns;
    entry.record.max_event_ts = page.header.max_timestamp_ns;
    entry.record.event_count = page.header.event_count;
    entry.record.page_digest = raw_digest;
    entry.record.merkle_root = root;
    if (config_.enable_pq_signatures) {
        PqKeyPair kp = pq_generate_keypair(derive_lsn_pq_seed(config_.pq_seed, lsn));
        const auto sig = pq_sign(root, kp.priv);
        if (!sig.has_value()) {
            return unexpected(sig.error());
        }
        entry.record.has_pq_signature = true;
        entry.record.pq_sig = *sig;
    }
    entry.wal_flags = config_.enable_compression
                          ? static_cast<std::uint16_t>(kPageFlagLz4)
                          : std::uint16_t{0};
    entry.encoded_payload = std::move(encoded);

    pending_.push_back(std::move(entry));

    if (pending_.size() >= config_.group_commit_max_pages) {
        const RingStatus st = flush_group_commit();
        if (st != RingStatus::Ok) {
            return unexpected(st);
        }
    }
    return lsn;
}

RingStatus DurableColumnarStore::flush_group_commit() {
    if (!is_open_) {
        return RingStatus::Closed;
    }
    if (pending_.empty()) {
        return RingStatus::Ok;
    }

    // Serialize all pending WAL records into one contiguous group-commit buffer.
    std::vector<std::uint8_t> batch_buf;
    std::vector<WalIndexEntry> pending_idx;
    pending_idx.reserve(pending_.size());

    std::uint64_t rel_offset = wal_valid_bytes_;
    for (const auto& entry : pending_) {
        WalRecordHeader hdr{};
        hdr.magic = kWalMagic;
        hdr.version = 1u;
        hdr.flags = entry.wal_flags;
        hdr.lsn = entry.record.lsn;
        hdr.commit_timestamp_ns = entry.record.commit_timestamp_ns;
        hdr.min_event_ts = entry.record.min_event_ts;
        hdr.max_event_ts = entry.record.max_event_ts;
        hdr.event_count = entry.record.event_count;
        hdr.payload_bytes = static_cast<std::uint64_t>(entry.encoded_payload.size());
        hdr.payload_crc32c =
            crc32c(entry.encoded_payload.data(), entry.encoded_payload.size());
        hdr.has_pq_signature = entry.record.has_pq_signature ? 1u : 0u;
        hdr.page_digest = entry.record.page_digest;
        hdr.merkle_root = entry.record.merkle_root;
        hdr.header_crc32c = 0;
        hdr.header_crc32c = crc32c(&hdr, sizeof(WalRecordHeader));

        const std::size_t rec_start = batch_buf.size();
        batch_buf.resize(rec_start + sizeof(WalRecordHeader));
        std::memcpy(batch_buf.data() + rec_start, &hdr, sizeof(WalRecordHeader));

        if (entry.record.has_pq_signature) {
            const std::size_t sig_off = batch_buf.size();
            batch_buf.resize(sig_off + sizeof(PqSignature));
            std::memcpy(batch_buf.data() + sig_off, &entry.record.pq_sig, sizeof(PqSignature));
        }

        const std::uint64_t payload_file_offset = rel_offset + batch_buf.size() - rec_start;
        const std::size_t pay_off = batch_buf.size();
        batch_buf.resize(pay_off + entry.encoded_payload.size());
        std::memcpy(batch_buf.data() + pay_off,
                    entry.encoded_payload.data(),
                    entry.encoded_payload.size());

        pending_idx.push_back(WalIndexEntry{
            payload_file_offset,
            static_cast<std::uint64_t>(entry.encoded_payload.size()),
            entry.wal_flags});

        rel_offset += (batch_buf.size() - rec_start);
    }

    std::size_t acks = 0;
    const bool primary_ok = append_and_fsync_file(primary_wal_path_, batch_buf);
    if (primary_ok) {
        ++acks;
    }

    for (const std::string& rep_dir : config_.sync_replica_dirs) {
        if (rep_dir.empty()) {
            continue;
        }
        const std::string rep_wal = (std::filesystem::path(rep_dir) / "wal.log").string();
        if (append_and_fsync_file(rep_wal, batch_buf)) {
            ++acks;
        }
    }

    if (!primary_ok || acks < config_.sync_quorum) {
        // Roll back primary WAL to `wal_valid_bytes_` so failed quorum leaves zero partial commit.
        std::error_code ec;
        if (std::filesystem::exists(primary_wal_path_, ec)) {
            std::filesystem::resize_file(primary_wal_path_, wal_valid_bytes_, ec);
        }
        pending_.clear();
        last_error_ = "Synchronous replication quorum not met during group commit";
        return RingStatus::IoError;
    }

    wal_valid_bytes_ += batch_buf.size();
    for (std::size_t i = 0; i < pending_.size(); ++i) {
        (void)merkle_.append_leaf_digest(pending_[i].record.page_digest);
        history_.push_back(pending_[i].record);
        wal_index_.push_back(pending_idx[i]);
    }
    next_lsn_ += pending_.size();
    pending_.clear();
    return RingStatus::Ok;
}

Expected<std::size_t, RingStatus> DurableColumnarStore::recover() {
    if (!is_open_) {
        return unexpected(RingStatus::Closed);
    }

    merkle_.clear();
    history_.clear();
    wal_index_.clear();
    wal_valid_bytes_ = 0;
    next_lsn_ = 1;

    std::error_code ec;
    if (!std::filesystem::exists(primary_wal_path_, ec)) {
        return std::size_t{0};
    }

    const std::uint64_t file_size = std::filesystem::file_size(primary_wal_path_, ec);
    if (ec) {
        last_error_ = "file_size failed on WAL: " + ec.message();
        return unexpected(RingStatus::IoError);
    }

    std::ifstream ifs(primary_wal_path_, std::ios::binary);
    if (!ifs.is_open()) {
        last_error_ = "Failed to open primary WAL for recovery";
        return unexpected(RingStatus::IoError);
    }

    std::uint64_t offset = 0;
    std::vector<std::uint8_t> payload_buf;

    while (offset + sizeof(WalRecordHeader) <= file_size) {
        ifs.seekg(static_cast<std::streamoff>(offset));
        WalRecordHeader hdr{};
        if (!ifs.read(reinterpret_cast<char*>(&hdr), sizeof(WalRecordHeader))) {
            break;
        }
        if (hdr.magic != kWalMagic || hdr.version != 1u ||
            hdr.payload_bytes == 0 ||
            hdr.payload_bytes > sizeof(ColumnarPage) * 2U) {
            break;
        }

        WalRecordHeader hdr_check = hdr;
        hdr_check.header_crc32c = 0;
        if (crc32c(&hdr_check, sizeof(WalRecordHeader)) != hdr.header_crc32c) {
            break;
        }

        const std::uint64_t sig_bytes =
            (hdr.has_pq_signature != 0u) ? sizeof(PqSignature) : 0ULL;
        if (offset + sizeof(WalRecordHeader) + sig_bytes + hdr.payload_bytes > file_size) {
            break; // Torn crash tail record
        }

        PqSignature sig{};
        if (hdr.has_pq_signature != 0u) {
            if (!ifs.read(reinterpret_cast<char*>(&sig), sizeof(PqSignature))) {
                break;
            }
        }

        payload_buf.resize(static_cast<std::size_t>(hdr.payload_bytes));
        if (!ifs.read(reinterpret_cast<char*>(payload_buf.data()),
                      static_cast<std::streamsize>(hdr.payload_bytes))) {
            break;
        }

        if (crc32c(payload_buf.data(), payload_buf.size()) != hdr.payload_crc32c) {
            break;
        }

        const Sha256Digest actual_digest = sha256(payload_buf.data(), payload_buf.size());
        if (!constant_time_equal(actual_digest, hdr.page_digest)) {
            break;
        }

        PageMerkleTree check_tree = merkle_;
        (void)check_tree.append_leaf_digest(actual_digest);
        const Sha256Digest expected_root = check_tree.root();
        if (!constant_time_equal(expected_root, hdr.merkle_root)) {
            break;
        }

        if (hdr.has_pq_signature != 0u) {
            const PqKeyPair kp =
                pq_generate_keypair(derive_lsn_pq_seed(config_.pq_seed, hdr.lsn));
            if (!pq_verify(hdr.merkle_root, sig, kp.pub)) {
                break;
            }
        }

        // Valid committed record — accept into recovered history.
        merkle_ = std::move(check_tree);

        CommitRecord rec;
        rec.lsn = hdr.lsn;
        rec.commit_timestamp_ns = hdr.commit_timestamp_ns;
        rec.min_event_ts = hdr.min_event_ts;
        rec.max_event_ts = hdr.max_event_ts;
        rec.event_count = hdr.event_count;
        rec.page_digest = hdr.page_digest;
        rec.merkle_root = hdr.merkle_root;
        rec.has_pq_signature = hdr.has_pq_signature != 0u;
        rec.pq_sig = sig;
        history_.push_back(std::move(rec));

        const std::uint64_t payload_offset =
            offset + sizeof(WalRecordHeader) + sig_bytes;
        wal_index_.push_back(WalIndexEntry{payload_offset, hdr.payload_bytes, hdr.flags});

        offset = payload_offset + hdr.payload_bytes;
        next_lsn_ = hdr.lsn + 1;
        wal_valid_bytes_ = offset;
    }

    ifs.close();
    if (wal_valid_bytes_ < file_size) {
        std::filesystem::resize_file(primary_wal_path_, wal_valid_bytes_, ec);
    }

    return history_.size();
}

Expected<std::size_t, RingStatus> DurableColumnarStore::recover_to_lsn(
    std::uint64_t target_lsn, const std::string& out_crng_path) const {
    if (!is_open_) {
        return unexpected(RingStatus::Closed);
    }
    std::ifstream ifs(primary_wal_path_, std::ios::binary);
    if (!ifs.is_open() && !history_.empty()) {
        return unexpected(RingStatus::IoError);
    }
    std::ofstream ofs(out_crng_path, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
        return unexpected(RingStatus::IoError);
    }

    std::size_t exported = 0;
    std::vector<std::uint8_t> buf;
    for (std::size_t i = 0; i < history_.size(); ++i) {
        if (history_[i].lsn > target_lsn) {
            break;
        }
        const auto& idx = wal_index_[i];
        buf.resize(static_cast<std::size_t>(idx.payload_bytes));
        ifs.seekg(static_cast<std::streamoff>(idx.file_offset));
        if (!ifs.read(reinterpret_cast<char*>(buf.data()),
                      static_cast<std::streamsize>(buf.size()))) {
            return unexpected(RingStatus::IoError);
        }
        const Sha256Digest d = sha256(buf.data(), buf.size());
        if (!constant_time_equal(d, history_[i].page_digest)) {
            return unexpected(RingStatus::CorruptedPage);
        }
        ofs.write(reinterpret_cast<const char*>(buf.data()),
                  static_cast<std::streamsize>(buf.size()));
        if (!ofs.good()) {
            return unexpected(RingStatus::IoError);
        }
        ++exported;
    }
    ofs.flush();
    return exported;
}

Expected<std::size_t, RingStatus> DurableColumnarStore::recover_to_timestamp(
    std::uint64_t target_commit_ts_ns, const std::string& out_crng_path) const {
    if (!is_open_) {
        return unexpected(RingStatus::Closed);
    }
    std::uint64_t max_lsn = 0;
    for (const auto& rec : history_) {
        if (rec.commit_timestamp_ns <= target_commit_ts_ns) {
            max_lsn = rec.lsn;
        } else {
            break;
        }
    }
    return recover_to_lsn(max_lsn, out_crng_path);
}

bool DurableColumnarStore::verify_commit_pq_signature(
    std::size_t commit_index) const noexcept {
    if (commit_index >= history_.size()) {
        return false;
    }
    const CommitRecord& rec = history_[commit_index];
    if (!rec.has_pq_signature) {
        return false;
    }
    const PqKeyPair kp =
        pq_generate_keypair(derive_lsn_pq_seed(config_.pq_seed, rec.lsn));
    return pq_verify(rec.merkle_root, rec.pq_sig, kp.pub);
}

} // namespace chronoring
