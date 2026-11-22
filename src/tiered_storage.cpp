#include "chronoring/tiered_storage.hpp"
#include "chronoring/columnar_sink.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <utility>

namespace chronoring {

namespace {

inline constexpr std::uint32_t kGeoShardMagic = 0x47454F53u; // "GEOS"

struct GeoShardHeader {
    std::uint32_t magic{kGeoShardMagic};
    std::uint16_t version{1u};
    std::uint16_t shard_index{0};
    std::uint32_t k_data{0};
    std::uint32_t m_parity{0};
    std::uint64_t page_id{0};
    std::uint64_t original_size{0};
    std::uint64_t shard_size{0};
    std::uint32_t shard_crc32c{0};
    std::uint32_t reserved{0};
    Sha256Digest payload_sha256{};
    Sha256Digest merkle_root{};
};

static_assert(sizeof(GeoShardHeader) == 112);
static_assert(std::is_trivially_copyable_v<GeoShardHeader>);

std::filesystem::path page_file_path(const std::string& dir, std::uint64_t page_id) {
    return std::filesystem::path(dir) / ("page_" + std::to_string(page_id) + ".bin");
}

bool write_atomic_bytes(const std::filesystem::path& target,
                        std::span<const std::uint8_t> data) noexcept {
    std::error_code ec;
    if (target.has_parent_path()) {
        std::filesystem::create_directories(target.parent_path(), ec);
        if (ec) {
            return false;
        }
    }
    const std::filesystem::path tmp = target.string() + ".tmp";
    {
        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
        if (!ofs.is_open()) {
            return false;
        }
        if (!data.empty()) {
            ofs.write(reinterpret_cast<const char*>(data.data()),
                      static_cast<std::streamsize>(data.size()));
        }
        ofs.flush();
        if (!ofs.good()) {
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    std::filesystem::rename(tmp, target, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

Expected<std::vector<std::uint8_t>, RingStatus> read_all_bytes(
    const std::filesystem::path& path) {
    std::error_code ec;
    const auto sz = std::filesystem::file_size(path, ec);
    if (ec) {
        return unexpected(RingStatus::IoError);
    }
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) {
        return unexpected(RingStatus::IoError);
    }
    std::vector<std::uint8_t> out(static_cast<std::size_t>(sz));
    if (!out.empty()) {
        if (!ifs.read(reinterpret_cast<char*>(out.data()),
                      static_cast<std::streamsize>(out.size()))) {
            return unexpected(RingStatus::IoError);
        }
    }
    return out;
}

std::string sanitized_key(const std::string& key) {
    std::string out = key;
    for (char& ch : out) {
        if (ch == '\\') {
            ch = '/';
        }
    }
    while (!out.empty() && out.front() == '/') {
        out.erase(out.begin());
    }
    return out;
}

} // namespace

// ============================================================================
// LocalObjectStore
// ============================================================================

LocalObjectStore::LocalObjectStore(std::string root_dir, std::string endpoint_label)
    : root_dir_(std::move(root_dir)), endpoint_label_(std::move(endpoint_label)) {
    std::error_code ec;
    std::filesystem::create_directories(root_dir_, ec);
}

RingStatus LocalObjectStore::put_object(const std::string& key,
                                        std::span<const std::uint8_t> data,
                                        const Sha256Digest& expected_etag) {
    if (offline_) {
        return RingStatus::IoError;
    }
    if (key.empty()) {
        return RingStatus::InvalidArgument;
    }
    const Sha256Digest actual = sha256(data.data(), data.size());
    if (!constant_time_equal(actual, expected_etag)) {
        return RingStatus::CorruptedPage;
    }

    const std::filesystem::path obj_path =
        std::filesystem::path(root_dir_) / sanitized_key(key);
    if (!write_atomic_bytes(obj_path, data)) {
        return RingStatus::IoError;
    }
    const std::filesystem::path etag_path = obj_path.string() + ".sha256";
    if (!write_atomic_bytes(etag_path, std::span<const std::uint8_t>(actual))) {
        return RingStatus::IoError;
    }
    return RingStatus::Ok;
}

Expected<std::vector<std::uint8_t>, RingStatus> LocalObjectStore::get_object(
    const std::string& key) const {
    if (offline_) {
        return unexpected(RingStatus::IoError);
    }
    const std::filesystem::path obj_path =
        std::filesystem::path(root_dir_) / sanitized_key(key);
    auto bytes = read_all_bytes(obj_path);
    if (!bytes.has_value()) {
        return unexpected(bytes.error());
    }
    const std::filesystem::path etag_path = obj_path.string() + ".sha256";
    auto etag_bytes = read_all_bytes(etag_path);
    if (etag_bytes.has_value() && etag_bytes->size() == sizeof(Sha256Digest)) {
        Sha256Digest expected{};
        std::memcpy(expected.data(), etag_bytes->data(), expected.size());
        const Sha256Digest actual = sha256(bytes->data(), bytes->size());
        if (!constant_time_equal(actual, expected)) {
            return unexpected(RingStatus::CorruptedPage);
        }
    }
    return bytes;
}

Expected<ObjectMeta, RingStatus> LocalObjectStore::head_object(
    const std::string& key) const {
    if (offline_) {
        return unexpected(RingStatus::IoError);
    }
    const std::filesystem::path obj_path =
        std::filesystem::path(root_dir_) / sanitized_key(key);
    std::error_code ec;
    if (!std::filesystem::exists(obj_path, ec)) {
        return unexpected(RingStatus::Empty);
    }
    ObjectMeta meta;
    meta.key = key;
    meta.size_bytes = static_cast<std::size_t>(std::filesystem::file_size(obj_path, ec));
    const std::filesystem::path etag_path = obj_path.string() + ".sha256";
    auto etag_bytes = read_all_bytes(etag_path);
    if (etag_bytes.has_value() && etag_bytes->size() == sizeof(Sha256Digest)) {
        std::memcpy(meta.etag.data(), etag_bytes->data(), meta.etag.size());
    }
    return meta;
}

std::vector<std::string> LocalObjectStore::list_objects(const std::string& prefix) const {
    std::vector<std::string> keys;
    if (offline_) {
        return keys;
    }
    std::error_code ec;
    const std::filesystem::path base(root_dir_);
    if (!std::filesystem::exists(base, ec)) {
        return keys;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(base, ec)) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        std::string rel =
            std::filesystem::relative(entry.path(), base, ec).generic_string();
        if (rel.size() >= 7 && rel.compare(rel.size() - 7, 7, ".sha256") == 0) {
            continue;
        }
        if (prefix.empty() || rel.compare(0, prefix.size(), prefix) == 0) {
            keys.push_back(std::move(rel));
        }
    }
    std::sort(keys.begin(), keys.end());
    return keys;
}

RingStatus LocalObjectStore::delete_object(const std::string& key) {
    if (offline_) {
        return RingStatus::IoError;
    }
    const std::filesystem::path obj_path =
        std::filesystem::path(root_dir_) / sanitized_key(key);
    std::error_code ec;
    std::filesystem::remove(obj_path, ec);
    std::filesystem::remove(obj_path.string() + ".sha256", ec);
    return RingStatus::Ok;
}

// ============================================================================
// TieredStorageManager
// ============================================================================

TieredStorageManager::TieredStorageManager(std::string hot_nvme_dir,
                                           std::string warm_ssd_dir,
                                           ObjectStoreClient* cold_object_store,
                                           TierPolicy policy)
    : hot_dir_(std::move(hot_nvme_dir)),
      warm_dir_(std::move(warm_ssd_dir)),
      cold_store_(cold_object_store),
      policy_(policy) {
    std::error_code ec;
    if (!hot_dir_.empty()) {
        std::filesystem::create_directories(hot_dir_, ec);
    }
    if (!warm_dir_.empty()) {
        std::filesystem::create_directories(warm_dir_, ec);
    }
}

RingStatus TieredStorageManager::put_page(std::uint64_t page_id,
                                          std::span<const std::uint8_t> bytes) {
    if (hot_dir_.empty() || bytes.empty()) {
        last_error_ = "TieredStorageManager requires non-empty hot_dir and data";
        return RingStatus::InvalidArgument;
    }
    const Sha256Digest digest = sha256(bytes.data(), bytes.size());
    const std::filesystem::path dst = page_file_path(hot_dir_, page_id);
    if (!write_atomic_bytes(dst, bytes)) {
        last_error_ = "Failed writing page to Hot NVMe tier";
        return RingStatus::IoError;
    }

    locations_[page_id] = StorageTier::HotNvme;
    digests_[page_id] = digest;
    hot_queue_.push_back(page_id);
    return run_tiering_pass();
}

RingStatus TieredStorageManager::run_tiering_pass() {
    std::error_code ec;

    // Hot (NVMe/Optane) -> Warm (SSD)
    while (hot_queue_.size() > policy_.hot_max_pages && !warm_dir_.empty()) {
        const std::uint64_t victim_id = hot_queue_.front();
        hot_queue_.pop_front();

        const std::filesystem::path src = page_file_path(hot_dir_, victim_id);
        const std::filesystem::path dst = page_file_path(warm_dir_, victim_id);
        auto data = read_all_bytes(src);
        if (!data.has_value()) {
            last_error_ = "Failed reading page from Hot tier during migration";
            return data.error();
        }
        const Sha256Digest actual = sha256(data->data(), data->size());
        if (!constant_time_equal(actual, digests_[victim_id])) {
            last_error_ = "Checksum mismatch migrating Hot -> Warm";
            return RingStatus::CorruptedPage;
        }
        if (!write_atomic_bytes(dst, *data)) {
            last_error_ = "Failed writing page to Warm SSD tier";
            return RingStatus::IoError;
        }
        std::filesystem::remove(src, ec);
        locations_[victim_id] = StorageTier::WarmSsd;
        warm_queue_.push_back(victim_id);
    }

    // Warm (SSD) -> Cold (S3/MinIO/Ceph Object Store)
    while (warm_queue_.size() > policy_.warm_max_pages && cold_store_ != nullptr) {
        const std::uint64_t victim_id = warm_queue_.front();
        warm_queue_.pop_front();

        const std::filesystem::path src = page_file_path(warm_dir_, victim_id);
        auto data = read_all_bytes(src);
        if (!data.has_value()) {
            last_error_ = "Failed reading page from Warm tier during migration";
            return data.error();
        }
        const std::string key = "tier_cold/page_" + std::to_string(victim_id) + ".bin";
        const RingStatus st =
            cold_store_->put_object(key, *data, digests_[victim_id]);
        if (st != RingStatus::Ok) {
            last_error_ = "Failed uploading page to Cold object store";
            return st;
        }
        std::filesystem::remove(src, ec);
        locations_[victim_id] = StorageTier::ColdObject;
        cold_queue_.push_back(victim_id);
    }

    return RingStatus::Ok;
}

Expected<std::vector<std::uint8_t>, RingStatus> TieredStorageManager::get_page(
    std::uint64_t page_id) const {
    const auto it = locations_.find(page_id);
    if (it == locations_.end()) {
        return unexpected(RingStatus::Empty);
    }

    Expected<std::vector<std::uint8_t>, RingStatus> res = unexpected(RingStatus::Empty);
    switch (it->second) {
    case StorageTier::HotNvme:
        res = read_all_bytes(page_file_path(hot_dir_, page_id));
        break;
    case StorageTier::WarmSsd:
        res = read_all_bytes(page_file_path(warm_dir_, page_id));
        break;
    case StorageTier::ColdObject:
        if (cold_store_ == nullptr) {
            return unexpected(RingStatus::IoError);
        }
        res = cold_store_->get_object("tier_cold/page_" + std::to_string(page_id) + ".bin");
        break;
    }

    if (!res.has_value()) {
        return res;
    }
    const auto dig_it = digests_.find(page_id);
    if (dig_it != digests_.end()) {
        const Sha256Digest actual = sha256(res->data(), res->size());
        if (!constant_time_equal(actual, dig_it->second)) {
            return unexpected(RingStatus::CorruptedPage);
        }
    }
    return res;
}

Expected<StorageTier, RingStatus> TieredStorageManager::locate_page(
    std::uint64_t page_id) const noexcept {
    const auto it = locations_.find(page_id);
    if (it == locations_.end()) {
        return unexpected(RingStatus::Empty);
    }
    return it->second;
}

std::size_t TieredStorageManager::count_in_tier(StorageTier tier) const noexcept {
    switch (tier) {
    case StorageTier::HotNvme:
        return hot_queue_.size();
    case StorageTier::WarmSsd:
        return warm_queue_.size();
    case StorageTier::ColdObject:
        return cold_queue_.size();
    }
    return 0;
}

// ============================================================================
// GeoReplicatedBackup
// ============================================================================

GeoReplicatedBackup::GeoReplicatedBackup(std::size_t k_data,
                                         std::size_t m_parity,
                                         std::vector<ObjectStoreClient*> region_stores)
    : coder_(k_data, m_parity), regions_(std::move(region_stores)) {}

RingStatus GeoReplicatedBackup::backup_page(std::uint64_t page_id,
                                            std::span<const std::uint8_t> payload,
                                            const Sha256Digest& merkle_root) {
    if (!is_valid() || payload.empty()) {
        return RingStatus::InvalidArgument;
    }

    const auto stripe_res = coder_.encode(payload);
    if (!stripe_res.has_value()) {
        return stripe_res.error();
    }
    const EncodedStripe& stripe = *stripe_res;
    const Sha256Digest payload_hash = sha256(payload.data(), payload.size());
    const std::size_t total_shards = stripe.shards.size();

    std::size_t uploaded = 0;
    for (std::size_t s = 0; s < total_shards; ++s) {
        if (regions_[s] == nullptr) {
            continue;
        }
        GeoShardHeader hdr{};
        hdr.magic = kGeoShardMagic;
        hdr.version = 1u;
        hdr.shard_index = static_cast<std::uint16_t>(s);
        hdr.k_data = static_cast<std::uint32_t>(stripe.k_data);
        hdr.m_parity = static_cast<std::uint32_t>(stripe.m_parity);
        hdr.page_id = page_id;
        hdr.original_size = static_cast<std::uint64_t>(stripe.original_size);
        hdr.shard_size = static_cast<std::uint64_t>(stripe.shard_size);
        hdr.shard_crc32c = stripe.shard_crc32c[s];
        hdr.payload_sha256 = payload_hash;
        hdr.merkle_root = merkle_root;

        std::vector<std::uint8_t> blob(sizeof(GeoShardHeader) + stripe.shard_size);
        std::memcpy(blob.data(), &hdr, sizeof(GeoShardHeader));
        std::memcpy(blob.data() + sizeof(GeoShardHeader),
                    stripe.shards[s].data(),
                    stripe.shard_size);

        const Sha256Digest blob_etag = sha256(blob.data(), blob.size());
        const std::string key =
            "geo/page_" + std::to_string(page_id) + "_s" + std::to_string(s) + ".rs";
        if (regions_[s]->put_object(key, blob, blob_etag) == RingStatus::Ok) {
            ++uploaded;
        }
    }

    return (uploaded >= coder_.k_data()) ? RingStatus::Ok : RingStatus::IoError;
}

Expected<std::vector<std::uint8_t>, RingStatus> GeoReplicatedBackup::restore_page(
    std::uint64_t page_id) const {
    if (!is_valid()) {
        return unexpected(RingStatus::InvalidArgument);
    }

    const std::size_t total_shards = coder_.k_data() + coder_.m_parity();
    EncodedStripe stripe;
    stripe.k_data = coder_.k_data();
    stripe.m_parity = coder_.m_parity();
    stripe.shards.resize(total_shards);
    stripe.shard_crc32c.resize(total_shards, 0);

    std::vector<std::uint8_t> present(total_shards, 0);
    bool have_meta = false;
    Sha256Digest expected_payload_sha{};

    for (std::size_t s = 0; s < total_shards; ++s) {
        if (regions_[s] == nullptr) {
            continue;
        }
        const std::string key =
            "geo/page_" + std::to_string(page_id) + "_s" + std::to_string(s) + ".rs";
        const auto obj = regions_[s]->get_object(key);
        if (!obj.has_value() || obj->size() <= sizeof(GeoShardHeader)) {
            continue;
        }

        GeoShardHeader hdr{};
        std::memcpy(&hdr, obj->data(), sizeof(GeoShardHeader));
        if (hdr.magic != kGeoShardMagic || hdr.version != 1u ||
            hdr.shard_index != s || hdr.page_id != page_id ||
            hdr.k_data != coder_.k_data() || hdr.m_parity != coder_.m_parity() ||
            obj->size() != sizeof(GeoShardHeader) + hdr.shard_size) {
            continue;
        }

        if (!have_meta) {
            stripe.original_size = static_cast<std::size_t>(hdr.original_size);
            stripe.shard_size = static_cast<std::size_t>(hdr.shard_size);
            expected_payload_sha = hdr.payload_sha256;
            have_meta = true;
        } else if (hdr.original_size != stripe.original_size ||
                   hdr.shard_size != stripe.shard_size ||
                   !constant_time_equal(hdr.payload_sha256, expected_payload_sha)) {
            continue;
        }

        stripe.shards[s].assign(
            obj->begin() + static_cast<std::ptrdiff_t>(sizeof(GeoShardHeader)), obj->end());
        stripe.shard_crc32c[s] = hdr.shard_crc32c;
        present[s] = 1;
    }

    if (!have_meta) {
        return unexpected(RingStatus::Empty);
    }
    for (std::size_t s = 0; s < total_shards; ++s) {
        if (present[s] == 0) {
            stripe.shards[s].assign(stripe.shard_size, 0);
        }
    }

    auto rec = coder_.reconstruct(stripe, present);
    if (!rec.has_value()) {
        return rec;
    }

    const Sha256Digest actual_sha = sha256(rec->data(), rec->size());
    if (!constant_time_equal(actual_sha, expected_payload_sha)) {
        return unexpected(RingStatus::CorruptedPage);
    }
    return rec;
}

} // namespace chronoring
