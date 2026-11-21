#pragma once

#include "chronoring/event_frame.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace chronoring {

[[nodiscard]] std::uint32_t crc32c_sw(const void* data, std::size_t len,
                                      std::uint32_t initial = 0) noexcept;

[[nodiscard]] std::uint32_t crc32c_hw(const void* data, std::size_t len,
                                      std::uint32_t initial = 0) noexcept;

[[nodiscard]] std::uint32_t crc32c(const void* data, std::size_t len,
                                   std::uint32_t initial = 0) noexcept;

void transpose_aos_to_soa(const EventFrame* frames, std::size_t count,
                          ColumnarPage& page) noexcept;

std::size_t transpose_soa_to_aos(const ColumnarPage& page, EventFrame* out_frames,
                                 std::size_t max_frames) noexcept;

void finalize_page_crc(ColumnarPage& page) noexcept;

[[nodiscard]] bool verify_page_crc(const ColumnarPage& page) noexcept;

class ColumnarSink {
public:
    ColumnarSink() noexcept = default;
    explicit ColumnarSink(std::string path, std::size_t initial_page_capacity = 64);
    ~ColumnarSink();

    ColumnarSink(const ColumnarSink&) = delete;
    ColumnarSink& operator=(const ColumnarSink&) = delete;
    ColumnarSink(ColumnarSink&& other) noexcept;
    ColumnarSink& operator=(ColumnarSink&& other) noexcept;

    [[nodiscard]] bool open(std::string path, std::size_t initial_page_capacity = 64);
    void close() noexcept;
    [[nodiscard]] bool flush() noexcept;

    [[nodiscard]] bool write(const EventFrame& frame) noexcept;
    [[nodiscard]] std::size_t write_batch(const EventFrame* frames, std::size_t count) noexcept;

    [[nodiscard]] bool is_open() const noexcept { return pages_ != nullptr; }
    [[nodiscard]] std::size_t pages_written() const noexcept { return committed_pages_; }
    [[nodiscard]] std::size_t events_written() const noexcept { return total_events_; }
    [[nodiscard]] const std::string& path() const noexcept { return path_; }
    [[nodiscard]] const std::string& last_error() const noexcept { return last_error_; }

private:
    bool map_pages(std::size_t new_page_capacity) noexcept;
    void unmap_pages() noexcept;
    bool commit_active_page() noexcept;
    void init_active_page_header() noexcept;

    std::string path_{};
    std::string last_error_{};

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

private:
    std::string path_{};
    std::string last_error_{};
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
