#pragma once

#include "chronoring/crypto_integrity.hpp"
#include "chronoring/erasure_coding.hpp"
#include "chronoring/status.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace chronoring {

enum class StorageTier : std::uint8_t {
    HotNvme    = 0,
    WarmSsd    = 1,
    ColdObject = 2
};

struct ObjectMeta {
    std::string key{};
    std::size_t size_bytes{0};
    Sha256Digest etag{};
};

// Abstract S3 / MinIO / Ceph RGW object-store interface.
class ObjectStoreClient {
public:
    virtual ~ObjectStoreClient() = default;

    [[nodiscard]] virtual std::string endpoint_name() const = 0;

    [[nodiscard]] virtual RingStatus put_object(
        const std::string& key,
        std::span<const std::uint8_t> data,
        const Sha256Digest& expected_etag) = 0;

    [[nodiscard]] virtual Expected<std::vector<std::uint8_t>, RingStatus> get_object(
        const std::string& key) const = 0;

    [[nodiscard]] virtual Expected<ObjectMeta, RingStatus> head_object(
        const std::string& key) const = 0;

    [[nodiscard]] virtual std::vector<std::string> list_objects(
        const std::string& prefix) const = 0;

    [[nodiscard]] virtual RingStatus delete_object(const std::string& key) = 0;
};

// Local filesystem-backed S3/MinIO/Ceph bucket emulator with atomic rename writes
// and SHA-256 ETag verification.
class LocalObjectStore final : public ObjectStoreClient {
public:
    explicit LocalObjectStore(std::string root_dir,
                              std::string endpoint_label = "local-s3");

    [[nodiscard]] std::string endpoint_name() const override { return endpoint_label_; }

    [[nodiscard]] RingStatus put_object(
        const std::string& key,
        std::span<const std::uint8_t> data,
        const Sha256Digest& expected_etag) override;

    [[nodiscard]] Expected<std::vector<std::uint8_t>, RingStatus> get_object(
        const std::string& key) const override;

    [[nodiscard]] Expected<ObjectMeta, RingStatus> head_object(
        const std::string& key) const override;

    [[nodiscard]] std::vector<std::string> list_objects(
        const std::string& prefix) const override;

    [[nodiscard]] RingStatus delete_object(const std::string& key) override;

    void set_offline(bool offline) noexcept { offline_ = offline; }
    [[nodiscard]] bool is_offline() const noexcept { return offline_; }

private:
    std::string root_dir_{};
    std::string endpoint_label_{};
    bool offline_{false};
};

struct TierPolicy {
    std::size_t hot_max_pages{4};
    std::size_t warm_max_pages{8};
};

// Automatic Hot (NVMe/Optane) -> Warm (SSD) -> Cold (S3/MinIO/Ceph) tiering manager.
class TieredStorageManager {
public:
    TieredStorageManager(std::string hot_nvme_dir,
                         std::string warm_ssd_dir,
                         ObjectStoreClient* cold_object_store,
                         TierPolicy policy = {});

    [[nodiscard]] RingStatus put_page(std::uint64_t page_id,
                                      std::span<const std::uint8_t> bytes);

    [[nodiscard]] Expected<std::vector<std::uint8_t>, RingStatus> get_page(
        std::uint64_t page_id) const;

    [[nodiscard]] Expected<StorageTier, RingStatus> locate_page(
        std::uint64_t page_id) const noexcept;

    [[nodiscard]] RingStatus run_tiering_pass();

    [[nodiscard]] std::size_t count_in_tier(StorageTier tier) const noexcept;
    [[nodiscard]] const std::string& last_error() const noexcept { return last_error_; }

private:
    std::string hot_dir_{};
    std::string warm_dir_{};
    ObjectStoreClient* cold_store_{nullptr};
    TierPolicy policy_{};

    std::deque<std::uint64_t> hot_queue_{};
    std::deque<std::uint64_t> warm_queue_{};
    std::deque<std::uint64_t> cold_queue_{};
    std::unordered_map<std::uint64_t, StorageTier> locations_{};
    std::unordered_map<std::uint64_t, Sha256Digest> digests_{};
    std::string last_error_{};
};

// Continuous erasure-coded geo-replicated backup manager across K+M object regions.
class GeoReplicatedBackup {
public:
    GeoReplicatedBackup(std::size_t k_data,
                        std::size_t m_parity,
                        std::vector<ObjectStoreClient*> region_stores);

    [[nodiscard]] bool is_valid() const noexcept {
        return coder_.is_valid() && regions_.size() >= (coder_.k_data() + coder_.m_parity());
    }

    [[nodiscard]] RingStatus backup_page(std::uint64_t page_id,
                                         std::span<const std::uint8_t> payload,
                                         const Sha256Digest& merkle_root);

    [[nodiscard]] Expected<std::vector<std::uint8_t>, RingStatus> restore_page(
        std::uint64_t page_id) const;

    [[nodiscard]] std::size_t region_count() const noexcept { return regions_.size(); }

private:
    ErasureCoder coder_;
    std::vector<ObjectStoreClient*> regions_{};
};

} // namespace chronoring
