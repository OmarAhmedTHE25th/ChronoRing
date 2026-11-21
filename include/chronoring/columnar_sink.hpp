#pragma once

#include "chronoring/event_frame.hpp"
#include "chronoring/simd_trigger.hpp"
#include "chronoring/status.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace chronoring {

[[nodiscard]] std::uint32_t crc32c_sw(const void* data, std::size_t len,
                                      std::uint32_t initial = 0) noexcept;

[[nodiscard]] std::uint32_t crc32c_hw(const void* data, std::size_t len,
                                      std::uint32_t initial = 0) noexcept;

[[nodiscard]] std::uint32_t crc32c(const void* data, std::size_t len,
                                   std::uint32_t initial = 0) noexcept;

void transpose_aos_to_soa(const EventFrame* frames, std::size_t count,
                          ColumnarPage& page,
                          std::uint16_t projection_mask = kColAll) noexcept;

std::size_t transpose_soa_to_aos(const ColumnarPage& page, EventFrame* out_frames,
                                 std::size_t max_frames) noexcept;

void finalize_page_crc(ColumnarPage& page) noexcept;

[[nodiscard]] bool verify_page_crc(const ColumnarPage& page) noexcept;

// Zero-copy memory-mapped AoS->SoA page writer with in-memory active page staging.
// Threading contract: single-writer-thread only; external synchronization is required
// if multiple threads invoke write/flush/close concurrently.
class ColumnarSink {
public:
    ColumnarSink() noexcept = default;
    explicit ColumnarSink(std::string path, std::size_t initial_page_capacity = 64,
                          std::uint16_t projection_mask = kColAll);
    ~ColumnarSink();

    ColumnarSink(const ColumnarSink&) = delete;
    ColumnarSink& operator=(const ColumnarSink&) = delete;
    ColumnarSink(ColumnarSink&& other) noexcept;
    ColumnarSink& operator=(ColumnarSink&& other) noexcept;

    [[nodiscard]] bool open(std::string path, std::size_t initial_page_capacity = 64,
                            std::uint16_t projection_mask = kColAll);
    void close() noexcept;
    [[nodiscard]] bool flush() noexcept;

    [[nodiscard]] bool write(const EventFrame& frame) noexcept;
    [[nodiscard]] std::size_t write_batch(const EventFrame* frames, std::size_t count) noexcept;
    [[nodiscard]] std::size_t write_batch(std::span<const EventFrame> frames) noexcept {
        return write_batch(frames.data(), frames.size());
    }

    [[nodiscard]] bool is_open() const noexcept { return pages_ != nullptr; }
    [[nodiscard]] std::size_t pages_written() const noexcept { return committed_pages_; }
    [[nodiscard]] std::size_t events_written() const noexcept { return total_events_; }
    [[nodiscard]] std::uint16_t projection_mask() const noexcept { return projection_mask_; }
    [[nodiscard]] const std::string& path() const noexcept { return path_; }
    [[nodiscard]] const std::string& last_error() const noexcept { return last_error_; }
    [[nodiscard]] RingStatus last_status() const noexcept { return last_status_; }

private:
    bool map_pages(std::size_t new_page_capacity) noexcept;
    void unmap_pages() noexcept;
    bool commit_active_page() noexcept;
    void init_active_page() noexcept;
    void write_projected_slot(std::size_t idx, const EventFrame& f) noexcept;

    std::string path_{};
    std::string last_error_{};
    RingStatus last_status_{RingStatus::Ok};
    std::uint16_t projection_mask_{kColAll};

    std::unique_ptr<ColumnarPage> staging_page_{};
    ColumnarPage* pages_{nullptr};
    std::size_t mapped_page_capacity_{0};
    std::size_t committed_pages_{0};
    std::size_t active_page_count_{0};
    std::size_t total_events_{0};

    std::uint64_t active_min_ts_{0};
    std::uint64_t active_max_ts_{0};
    std::uint64_t active_first_seq_{0};
    std::uint64_t active_last_seq_{0};

#ifdef _WIN32
    void* file_handle_{nullptr};
    void* mapping_handle_{nullptr};
#else
    int fd_{-1};
#endif
};

struct PageIndexEntry {
    std::size_t page_index{0};
    std::uint32_t event_count{0};
    std::uint64_t min_timestamp_ns{0};
    std::uint64_t max_timestamp_ns{0};
    std::uint64_t first_sequence_id{0};
    std::uint64_t last_sequence_id{0};
};

class ColumnarReader {
public:
    ColumnarReader() noexcept = default;
    explicit ColumnarReader(std::string path);
    ~ColumnarReader();

    ColumnarReader(const ColumnarReader&) = delete;
    ColumnarReader& operator=(const ColumnarReader&) = delete;
    ColumnarReader(ColumnarReader&& other) noexcept;
    ColumnarReader& operator=(ColumnarReader&& other) noexcept;

    [[nodiscard]] bool open(std::string path);
    void close() noexcept;

    [[nodiscard]] bool is_open() const noexcept { return mapped_base_ != nullptr; }
    [[nodiscard]] std::size_t num_pages() const noexcept { return num_pages_; }
    [[nodiscard]] std::size_t total_events() const noexcept;
    [[nodiscard]] const ColumnarPage& page(std::size_t index) const noexcept { return pages_[index]; }
    [[nodiscard]] bool verify_all_pages(std::size_t* corrupted_page_index = nullptr) const noexcept;
    [[nodiscard]] const std::string& last_error() const noexcept { return last_error_; }
    [[nodiscard]] RingStatus last_status() const noexcept { return last_status_; }

    [[nodiscard]] std::vector<PageIndexEntry> page_index() const;
    [[nodiscard]] std::vector<std::size_t> find_pages_by_timestamp(
        std::uint64_t min_ts, std::uint64_t max_ts) const;
    [[nodiscard]] Expected<std::size_t, RingStatus> find_page_by_sequence(
        std::uint64_t sequence_id) const noexcept;
    [[nodiscard]] Expected<std::size_t, RingStatus> replay_filtered(
        const TriggerConfig& cfg,
        SimdBackend backend = SimdBackend::Auto,
        std::uint64_t* out_total_scanned = nullptr) const noexcept;

private:
    std::string path_{};
    std::string last_error_{};
    RingStatus last_status_{RingStatus::Ok};
    void* mapped_base_{nullptr};
    const ColumnarPage* pages_{nullptr};
    std::size_t file_bytes_{0};
    std::size_t num_pages_{0};
    bool size_aligned_{true};

#ifdef _WIN32
    void* file_handle_{nullptr};
    void* mapping_handle_{nullptr};
#else
    int fd_{-1};
#endif
};

} // namespace chronoring
