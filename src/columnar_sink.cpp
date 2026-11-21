#include "chronoring/columnar_sink.hpp"
#include "chronoring/lz4_codec.hpp"
#include "chronoring/simd_trigger.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <limits>
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
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <nmmintrin.h>
#define CHRONORING_CRC_X86 1
#elif (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
#include <nmmintrin.h>
#define CHRONORING_CRC_X86 1
#endif

#if defined(__GNUC__) || defined(__clang__)
#if defined(__x86_64__) || defined(__i386__)
#define CHRONORING_CRC_TARGET_SSE42 __attribute__((target("sse4.2")))
#else
#define CHRONORING_CRC_TARGET_SSE42
#endif
#else
#define CHRONORING_CRC_TARGET_SSE42
#endif

namespace chronoring {

namespace {

constexpr std::uint32_t kCrc32cPoly = 0x82F63B78u;

constexpr std::array<std::uint32_t, 256> build_crc32c_table() noexcept {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t crc = i;
        for (int bit = 0; bit < 8; ++bit) {
            if ((crc & 1u) != 0u) {
                crc = (crc >> 1) ^ kCrc32cPoly;
            } else {
                crc >>= 1;
            }
        }
        table[i] = crc;
    }
    return table;
}

inline constexpr auto kCrc32cTable = build_crc32c_table();

void zero_unused_page_tail(ColumnarPage& page, std::size_t n) noexcept {
    if (n >= kColumnarPageEvents) {
        return;
    }
    const std::size_t rem = kColumnarPageEvents - n;
    std::memset(page.timestamp_ns.data() + n, 0, rem * sizeof(std::uint64_t));
    std::memset(page.sequence_id.data() + n, 0, rem * sizeof(std::uint64_t));
    std::memset(page.detector_id.data() + n, 0, rem * sizeof(std::uint32_t));
    std::memset(page.channel_id.data() + n, 0, rem * sizeof(std::uint32_t));
    std::memset(page.energy_mev.data() + n, 0, rem * sizeof(float));
    std::memset(page.timing_ps.data() + n, 0, rem * sizeof(float));
    std::memset(page.pos_x_mm.data() + n, 0, rem * sizeof(float));
    std::memset(page.pos_y_mm.data() + n, 0, rem * sizeof(float));
    std::memset(page.pos_z_mm.data() + n, 0, rem * sizeof(float));
    std::memset(page.pileup_score.data() + n, 0, rem * sizeof(float));
    std::memset(page.quality_flags.data() + n, 0, rem * sizeof(std::uint32_t));
    std::memset(page.multiplicity.data() + n, 0, rem * sizeof(std::uint16_t));
    std::memset(page.trigger_mask.data() + n, 0, rem * sizeof(std::uint16_t));
}

} // namespace

std::uint32_t crc32c_sw(const void* data, std::size_t len,
                        std::uint32_t initial) noexcept {
    if (data == nullptr || len == 0) {
        return initial;
    }
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::uint32_t crc = initial ^ 0xFFFFFFFFu;
    for (std::size_t i = 0; i < len; ++i) {
        crc = kCrc32cTable[(crc ^ bytes[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

CHRONORING_CRC_TARGET_SSE42
std::uint32_t crc32c_hw(const void* data, std::size_t len,
                        std::uint32_t initial) noexcept {
#ifdef CHRONORING_CRC_X86
    if (!cpu_supports_sse42()) {
        return crc32c_sw(data, len, initial);
    }
    if (data == nullptr || len == 0) {
        return initial;
    }
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::uint32_t crc = initial ^ 0xFFFFFFFFu;
    std::size_t offset = 0;

#if defined(__x86_64__) || defined(_M_X64)
    std::uint64_t crc64 = crc;
    while (offset + sizeof(std::uint64_t) <= len) {
        std::uint64_t chunk = 0;
        std::memcpy(&chunk, bytes + offset, sizeof(chunk));
        crc64 = static_cast<std::uint64_t>(
            _mm_crc32_u64(static_cast<unsigned long long>(crc64),
                          static_cast<unsigned long long>(chunk)));
        offset += sizeof(std::uint64_t);
    }
    crc = static_cast<std::uint32_t>(crc64);
#else
    while (offset + sizeof(std::uint32_t) <= len) {
        std::uint32_t chunk = 0;
        std::memcpy(&chunk, bytes + offset, sizeof(chunk));
        crc = _mm_crc32_u32(crc, chunk);
        offset += sizeof(std::uint32_t);
    }
#endif

    while (offset < len) {
        crc = _mm_crc32_u8(crc, bytes[offset]);
        ++offset;
    }
    return crc ^ 0xFFFFFFFFu;
#else
    return crc32c_sw(data, len, initial);
#endif
}

std::uint32_t crc32c(const void* data, std::size_t len,
                     std::uint32_t initial) noexcept {
    if (cpu_supports_sse42()) {
        return crc32c_hw(data, len, initial);
    }
    return crc32c_sw(data, len, initial);
}

void transpose_aos_to_soa(const EventFrame* frames, std::size_t count,
                          ColumnarPage& page, std::uint16_t projection_mask) noexcept {
    const std::size_t n =
        (frames == nullptr) ? 0 : std::min<std::size_t>(count, kColumnarPageEvents);
    const std::uint16_t mask =
        (projection_mask == 0u) ? static_cast<std::uint16_t>(kColAll) : projection_mask;

    page = ColumnarPage{};
    page.header.flags = mask;
    page.header.event_count = static_cast<std::uint32_t>(n);

    if (n == 0) {
        finalize_page_crc(page);
        return;
    }

    std::uint64_t min_ts = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t max_ts = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const EventFrame& f = frames[i];
        if ((mask & kColTimestamps) != 0u) {
            page.timestamp_ns[i] = f.timestamp_ns;
            page.sequence_id[i] = f.sequence_id;
        }
        if ((mask & kColDetectorChannel) != 0u) {
            page.detector_id[i] = f.detector_id;
            page.channel_id[i] = f.channel_id;
        }
        if ((mask & kColKinematics) != 0u) {
            page.energy_mev[i] = f.energy_mev;
            page.timing_ps[i] = f.timing_ps;
            page.pileup_score[i] = f.pileup_score;
        }
        if ((mask & kColPositions) != 0u) {
            page.pos_x_mm[i] = f.pos_x_mm;
            page.pos_y_mm[i] = f.pos_y_mm;
            page.pos_z_mm[i] = f.pos_z_mm;
        }
        if ((mask & kColQualityTrigger) != 0u) {
            page.quality_flags[i] = f.quality_flags;
            page.multiplicity[i] = f.multiplicity;
            page.trigger_mask[i] = f.trigger_mask;
        }

        min_ts = std::min(min_ts, f.timestamp_ns);
        max_ts = std::max(max_ts, f.timestamp_ns);
    }

    page.header.min_timestamp_ns = min_ts;
    page.header.max_timestamp_ns = max_ts;
    page.header.first_sequence_id = frames[0].sequence_id;
    page.header.last_sequence_id = frames[n - 1].sequence_id;

    zero_unused_page_tail(page, n);
    finalize_page_crc(page);
}

std::size_t transpose_soa_to_aos(const ColumnarPage& page, EventFrame* out_frames,
                                 std::size_t max_frames) noexcept {
    if (out_frames == nullptr || max_frames == 0) {
        return 0;
    }
    const std::size_t page_events =
        std::min<std::size_t>(page.header.event_count, kColumnarPageEvents);
    const std::size_t n = std::min(page_events, max_frames);

    for (std::size_t i = 0; i < n; ++i) {
        EventFrame& f = out_frames[i];
        f.timestamp_ns = page.timestamp_ns[i];
        f.sequence_id = page.sequence_id[i];
        f.detector_id = page.detector_id[i];
        f.channel_id = page.channel_id[i];
        f.energy_mev = page.energy_mev[i];
        f.timing_ps = page.timing_ps[i];
        f.pos_x_mm = page.pos_x_mm[i];
        f.pos_y_mm = page.pos_y_mm[i];
        f.pos_z_mm = page.pos_z_mm[i];
        f.pileup_score = page.pileup_score[i];
        f.quality_flags = page.quality_flags[i];
        f.multiplicity = page.multiplicity[i];
        f.trigger_mask = page.trigger_mask[i];
        f.payload_crc = 0;
        f.reserved = 0;
    }
    return n;
}

void finalize_page_crc(ColumnarPage& page) noexcept {
    const auto* payload =
        reinterpret_cast<const std::uint8_t*>(&page) + sizeof(ColumnarPageHeader);
    constexpr std::size_t kPayloadBytes = sizeof(ColumnarPage) - sizeof(ColumnarPageHeader);
    page.header.data_crc32c = crc32c(payload, kPayloadBytes);
    page.header.header_crc32c = 0;
    page.header.header_crc32c = crc32c(&page.header, sizeof(ColumnarPageHeader));
}

bool verify_page_crc(const ColumnarPage& page) noexcept {
    if (page.header.magic != kColumnarPageMagic ||
        page.header.version != kColumnarFormatVersion ||
        page.header.capacity != static_cast<std::uint32_t>(kColumnarPageEvents) ||
        page.header.event_count > kColumnarPageEvents) {
        return false;
    }

    ColumnarPageHeader hdr_copy = page.header;
    hdr_copy.header_crc32c = 0;
    const std::uint32_t expected_hdr_crc =
        crc32c(&hdr_copy, sizeof(ColumnarPageHeader));
    if (expected_hdr_crc != page.header.header_crc32c) {
        return false;
    }

    const auto* payload =
        reinterpret_cast<const std::uint8_t*>(&page) + sizeof(ColumnarPageHeader);
    constexpr std::size_t kPayloadBytes = sizeof(ColumnarPage) - sizeof(ColumnarPageHeader);
    const std::uint32_t expected_data_crc = crc32c(payload, kPayloadBytes);
    return expected_data_crc == page.header.data_crc32c;
}

// ============================================================================
// ColumnarSink
// ============================================================================

ColumnarSink::ColumnarSink(std::string path, std::size_t initial_page_capacity,
                           std::uint16_t projection_mask,
                           bool enable_compression) {
    (void)open(std::move(path), initial_page_capacity, projection_mask, enable_compression);
}

ColumnarSink::~ColumnarSink() {
    close();
}

ColumnarSink::ColumnarSink(ColumnarSink&& other) noexcept
    : path_(std::move(other.path_)),
      last_error_(std::move(other.last_error_)),
      last_status_(other.last_status_),
      projection_mask_(other.projection_mask_),
      enable_compression_(other.enable_compression_),
      staging_page_(std::move(other.staging_page_)),
      compress_scratch_(std::move(other.compress_scratch_)),
      pages_(other.pages_),
      mapped_page_capacity_(other.mapped_page_capacity_),
      committed_pages_(other.committed_pages_),
      committed_bytes_(other.committed_bytes_),
      active_page_count_(other.active_page_count_),
      total_events_(other.total_events_),
      active_min_ts_(other.active_min_ts_),
      active_max_ts_(other.active_max_ts_),
      active_first_seq_(other.active_first_seq_),
      active_last_seq_(other.active_last_seq_),
#ifdef _WIN32
      file_handle_(other.file_handle_),
      mapping_handle_(other.mapping_handle_)
#else
      fd_(other.fd_)
#endif
{
    other.pages_ = nullptr;
    other.mapped_page_capacity_ = 0;
    other.committed_pages_ = 0;
    other.committed_bytes_ = 0;
    other.active_page_count_ = 0;
    other.total_events_ = 0;
#ifdef _WIN32
    other.file_handle_ = nullptr;
    other.mapping_handle_ = nullptr;
#else
    other.fd_ = -1;
#endif
}

ColumnarSink& ColumnarSink::operator=(ColumnarSink&& other) noexcept {
    if (this != &other) {
        close();
        path_ = std::move(other.path_);
        last_error_ = std::move(other.last_error_);
        last_status_ = other.last_status_;
        projection_mask_ = other.projection_mask_;
        enable_compression_ = other.enable_compression_;
        staging_page_ = std::move(other.staging_page_);
        compress_scratch_ = std::move(other.compress_scratch_);
        pages_ = other.pages_;
        mapped_page_capacity_ = other.mapped_page_capacity_;
        committed_pages_ = other.committed_pages_;
        committed_bytes_ = other.committed_bytes_;
        active_page_count_ = other.active_page_count_;
        total_events_ = other.total_events_;
        active_min_ts_ = other.active_min_ts_;
        active_max_ts_ = other.active_max_ts_;
        active_first_seq_ = other.active_first_seq_;
        active_last_seq_ = other.active_last_seq_;
#ifdef _WIN32
        file_handle_ = other.file_handle_;
        mapping_handle_ = other.mapping_handle_;
        other.file_handle_ = nullptr;
        other.mapping_handle_ = nullptr;
#else
        fd_ = other.fd_;
        other.fd_ = -1;
#endif
        other.pages_ = nullptr;
        other.mapped_page_capacity_ = 0;
        other.committed_pages_ = 0;
        other.committed_bytes_ = 0;
        other.active_page_count_ = 0;
        other.total_events_ = 0;
    }
    return *this;
}

bool ColumnarSink::open(std::string path, std::size_t initial_page_capacity,
                        std::uint16_t projection_mask, bool enable_compression) {
    close();
    path_ = std::move(path);
    last_error_.clear();
    last_status_ = RingStatus::Ok;
    projection_mask_ =
        (projection_mask == 0u) ? static_cast<std::uint16_t>(kColAll) : projection_mask;
    enable_compression_ = enable_compression;
    committed_pages_ = 0;
    committed_bytes_ = 0;
    active_page_count_ = 0;
    total_events_ = 0;

    if (!staging_page_) {
        staging_page_ = std::make_unique<ColumnarPage>();
    }
    if (enable_compression_) {
        constexpr std::size_t kPayloadBytes = sizeof(ColumnarPage) - sizeof(ColumnarPageHeader);
        compress_scratch_.resize(lz4_compress_bound(kPayloadBytes));
    }

    const std::size_t cap = std::max<std::size_t>(initial_page_capacity, 4);

#ifdef _WIN32
    HANDLE fh = CreateFileA(path_.c_str(), GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fh == INVALID_HANDLE_VALUE) {
        last_status_ = RingStatus::IoError;
        last_error_ = "CreateFileA failed for ColumnarSink";
        emit_log(LogLevel::Error, last_status_, "columnar_sink", last_error_);
        return false;
    }
    file_handle_ = fh;
#else
    const int fd = ::open(path_.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        last_status_ = RingStatus::IoError;
        last_error_ = std::string("open failed for ColumnarSink: ") + std::strerror(errno);
        emit_log(LogLevel::Error, last_status_, "columnar_sink", last_error_);
        return false;
    }
    fd_ = fd;
#endif

    if (!map_pages(cap)) {
        close();
        return false;
    }
    init_active_page();
    return true;
}

bool ColumnarSink::map_pages(std::size_t new_page_capacity) noexcept {
    assert(new_page_capacity > 0);
    const std::size_t new_bytes = new_page_capacity * sizeof(ColumnarPage);
    if (mapped_page_capacity_ > 0 && new_page_capacity > mapped_page_capacity_) {
        emit_log(LogLevel::Info, RingStatus::Ok, "columnar_sink",
                 "Expanding mmap columnar page capacity", new_page_capacity);
    }
#ifdef _WIN32
    unmap_pages();
    LARGE_INTEGER li{};
    li.QuadPart = static_cast<LONGLONG>(new_bytes);
    if (!SetFilePointerEx(static_cast<HANDLE>(file_handle_), li, nullptr, FILE_BEGIN) ||
        !SetEndOfFile(static_cast<HANDLE>(file_handle_))) {
        last_status_ = RingStatus::IoError;
        last_error_ = "Failed to resize ColumnarSink file";
        emit_log(LogLevel::Error, last_status_, "columnar_sink", last_error_);
        return false;
    }
    HANDLE mh = CreateFileMappingA(static_cast<HANDLE>(file_handle_), nullptr,
                                   PAGE_READWRITE, 0, 0, nullptr);
    if (mh == nullptr) {
        last_status_ = RingStatus::MappingError;
        last_error_ = "CreateFileMappingA failed for ColumnarSink";
        emit_log(LogLevel::Error, last_status_, "columnar_sink", last_error_);
        return false;
    }
    void* view = MapViewOfFile(mh, FILE_MAP_ALL_ACCESS, 0, 0, new_bytes);
    if (view == nullptr) {
        CloseHandle(mh);
        last_status_ = RingStatus::MappingError;
        last_error_ = "MapViewOfFile failed for ColumnarSink";
        emit_log(LogLevel::Error, last_status_, "columnar_sink", last_error_);
        return false;
    }
    mapping_handle_ = mh;
    pages_ = static_cast<ColumnarPage*>(view);
    mapped_page_capacity_ = new_page_capacity;
    return true;
#else
    const std::size_t old_bytes = mapped_page_capacity_ * sizeof(ColumnarPage);
    if (pages_ != nullptr) {
        ::munmap(pages_, old_bytes);
        pages_ = nullptr;
    }
    if (::ftruncate(fd_, static_cast<off_t>(new_bytes)) != 0) {
        last_status_ = RingStatus::IoError;
        last_error_ = std::string("ftruncate failed: ") + std::strerror(errno);
        emit_log(LogLevel::Error, last_status_, "columnar_sink", last_error_);
        return false;
    }
    void* view = ::mmap(nullptr, new_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (view == MAP_FAILED) {
        last_status_ = RingStatus::MappingError;
        last_error_ = std::string("mmap failed: ") + std::strerror(errno);
        emit_log(LogLevel::Error, last_status_, "columnar_sink", last_error_);
        return false;
    }
    pages_ = static_cast<ColumnarPage*>(view);
    mapped_page_capacity_ = new_page_capacity;
    return true;
#endif
}

void ColumnarSink::unmap_pages() noexcept {
#ifdef _WIN32
    if (pages_ != nullptr) {
        UnmapViewOfFile(pages_);
        pages_ = nullptr;
    }
    if (mapping_handle_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(mapping_handle_));
        mapping_handle_ = nullptr;
    }
#else
    if (pages_ != nullptr) {
        ::munmap(pages_, mapped_page_capacity_ * sizeof(ColumnarPage));
        pages_ = nullptr;
    }
#endif
}

void ColumnarSink::init_active_page() noexcept {
    if (staging_page_) {
        *staging_page_ = ColumnarPage{};
        staging_page_->header.flags = projection_mask_;
    }
    active_page_count_ = 0;
    active_min_ts_ = std::numeric_limits<std::uint64_t>::max();
    active_max_ts_ = 0;
    active_first_seq_ = 0;
    active_last_seq_ = 0;
}

void ColumnarSink::write_projected_slot(std::size_t idx, const EventFrame& f) noexcept {
    assert(staging_page_ != nullptr);
    assert(idx < kColumnarPageEvents);
    ColumnarPage& page = *staging_page_;

    if ((projection_mask_ & kColTimestamps) != 0u) {
        page.timestamp_ns[idx] = f.timestamp_ns;
        page.sequence_id[idx] = f.sequence_id;
    }
    if ((projection_mask_ & kColDetectorChannel) != 0u) {
        page.detector_id[idx] = f.detector_id;
        page.channel_id[idx] = f.channel_id;
    }
    if ((projection_mask_ & kColKinematics) != 0u) {
        page.energy_mev[idx] = f.energy_mev;
        page.timing_ps[idx] = f.timing_ps;
        page.pileup_score[idx] = f.pileup_score;
    }
    if ((projection_mask_ & kColPositions) != 0u) {
        page.pos_x_mm[idx] = f.pos_x_mm;
        page.pos_y_mm[idx] = f.pos_y_mm;
        page.pos_z_mm[idx] = f.pos_z_mm;
    }
    if ((projection_mask_ & kColQualityTrigger) != 0u) {
        page.quality_flags[idx] = f.quality_flags;
        page.multiplicity[idx] = f.multiplicity;
        page.trigger_mask[idx] = f.trigger_mask;
    }
}

bool ColumnarSink::commit_active_page() noexcept {
    if (pages_ == nullptr || !staging_page_ || active_page_count_ == 0) {
        return true;
    }

    ColumnarPage& page = *staging_page_;
    page.header.magic = kColumnarPageMagic;
    page.header.version = kColumnarFormatVersion;
    page.header.flags = projection_mask_;
    page.header.event_count = static_cast<std::uint32_t>(active_page_count_);
    page.header.capacity = static_cast<std::uint32_t>(kColumnarPageEvents);
    page.header.min_timestamp_ns = active_min_ts_;
    page.header.max_timestamp_ns = active_max_ts_;
    page.header.first_sequence_id = active_first_seq_;
    page.header.last_sequence_id = active_last_seq_;
    page.header.compressed_bytes = 0;
    page.header.projection_mask = projection_mask_;

    constexpr std::size_t kPayloadBytes = sizeof(ColumnarPage) - sizeof(ColumnarPageHeader);
    std::size_t write_bytes = sizeof(ColumnarPage);
    bool wrote_compressed = false;

    if (enable_compression_ && !compress_scratch_.empty()) {
        zero_unused_page_tail(page, active_page_count_);
        const auto* payload =
            reinterpret_cast<const std::uint8_t*>(&page) + sizeof(ColumnarPageHeader);
        const std::size_t comp_len = lz4_compress(
            payload, kPayloadBytes, compress_scratch_.data(), compress_scratch_.size());
        if (comp_len > 0 && comp_len < kPayloadBytes) {
            page.header.flags = static_cast<std::uint16_t>(projection_mask_ | kPageFlagLz4);
            page.header.compressed_bytes = static_cast<std::uint32_t>(comp_len);
            page.header.data_crc32c = crc32c(compress_scratch_.data(), comp_len);
            page.header.header_crc32c = 0;
            page.header.header_crc32c = crc32c(&page.header, sizeof(ColumnarPageHeader));
            write_bytes = sizeof(ColumnarPageHeader) + comp_len;
            wrote_compressed = true;
        }
    }

    if (!wrote_compressed) {
        finalize_page_crc(page);
    }

    const std::size_t mapped_bytes = mapped_page_capacity_ * sizeof(ColumnarPage);
    if (committed_bytes_ + write_bytes > mapped_bytes) {
        if (!map_pages(std::max(mapped_page_capacity_ * 2, committed_pages_ + 2))) {
            return false;
        }
    }

    auto* dst = reinterpret_cast<std::uint8_t*>(pages_) + committed_bytes_;
    if (wrote_compressed) {
        std::memcpy(dst, &page.header, sizeof(ColumnarPageHeader));
        std::memcpy(dst + sizeof(ColumnarPageHeader),
                    compress_scratch_.data(), page.header.compressed_bytes);
    } else {
        std::memcpy(dst, &page, sizeof(ColumnarPage));
    }

    committed_bytes_ += write_bytes;
    ++committed_pages_;
    active_page_count_ = 0;
    init_active_page();
    return true;
}

bool ColumnarSink::write(const EventFrame& frame) noexcept {
    if (pages_ == nullptr || !staging_page_) {
        last_status_ = RingStatus::IoError;
        return false;
    }

    const std::size_t idx = active_page_count_;
    write_projected_slot(idx, frame);

    if (idx == 0) {
        active_min_ts_ = frame.timestamp_ns;
        active_max_ts_ = frame.timestamp_ns;
        active_first_seq_ = frame.sequence_id;
    } else {
        active_min_ts_ = std::min(active_min_ts_, frame.timestamp_ns);
        active_max_ts_ = std::max(active_max_ts_, frame.timestamp_ns);
    }
    active_last_seq_ = frame.sequence_id;

    ++active_page_count_;
    ++total_events_;

    if (active_page_count_ == kColumnarPageEvents) {
        return commit_active_page();
    }
    return true;
}

std::size_t ColumnarSink::write_batch(const EventFrame* frames,
                                      std::size_t count) noexcept {
    if (pages_ == nullptr || !staging_page_ || frames == nullptr || count == 0) {
        return 0;
    }
    std::size_t written = 0;
    while (written < count) {
        const std::size_t space = kColumnarPageEvents - active_page_count_;
        const std::size_t chunk = std::min(space, count - written);

        for (std::size_t k = 0; k < chunk; ++k) {
            const EventFrame& f = frames[written + k];
            const std::size_t idx = active_page_count_ + k;
            write_projected_slot(idx, f);

            if (active_page_count_ == 0 && k == 0) {
                active_min_ts_ = f.timestamp_ns;
                active_max_ts_ = f.timestamp_ns;
                active_first_seq_ = f.sequence_id;
            } else {
                active_min_ts_ = std::min(active_min_ts_, f.timestamp_ns);
                active_max_ts_ = std::max(active_max_ts_, f.timestamp_ns);
            }
            active_last_seq_ = f.sequence_id;
        }

        active_page_count_ += chunk;
        total_events_ += chunk;
        written += chunk;

        if (active_page_count_ == kColumnarPageEvents) {
            if (!commit_active_page()) {
                break;
            }
        }
    }
    return written;
}

bool ColumnarSink::flush() noexcept {
    if (pages_ == nullptr) {
        return false;
    }
    if (active_page_count_ > 0) {
        if (!commit_active_page()) {
            return false;
        }
    }
#ifdef _WIN32
    if (committed_bytes_ > 0) {
        if (!FlushViewOfFile(pages_, committed_bytes_)) {
            last_status_ = RingStatus::IoError;
            return false;
        }
    }
    return FlushFileBuffers(static_cast<HANDLE>(file_handle_)) != FALSE;
#else
    if (committed_bytes_ > 0) {
        if (::msync(pages_, committed_bytes_, MS_SYNC) != 0) {
            last_status_ = RingStatus::IoError;
            last_error_ = std::string("msync failed: ") + std::strerror(errno);
            emit_log(LogLevel::Error, last_status_, "columnar_sink", last_error_);
            return false;
        }
    }
    return true;
#endif
}

void ColumnarSink::close() noexcept {
    if (pages_ != nullptr) {
        (void)flush();
        unmap_pages();
    }
    const std::size_t final_bytes = committed_bytes_;
#ifdef _WIN32
    if (file_handle_ != nullptr) {
        LARGE_INTEGER li{};
        li.QuadPart = static_cast<LONGLONG>(final_bytes);
        SetFilePointerEx(static_cast<HANDLE>(file_handle_), li, nullptr, FILE_BEGIN);
        SetEndOfFile(static_cast<HANDLE>(file_handle_));
        CloseHandle(static_cast<HANDLE>(file_handle_));
        file_handle_ = nullptr;
    }
#else
    if (fd_ >= 0) {
        if (::ftruncate(fd_, static_cast<off_t>(final_bytes)) != 0) {
            last_status_ = RingStatus::IoError;
            last_error_ = std::string("ftruncate(close) failed: ") + std::strerror(errno);
        }
        ::close(fd_);
        fd_ = -1;
    }
#endif
    mapped_page_capacity_ = 0;
    active_page_count_ = 0;
}

// ============================================================================
// ColumnarReader
// ============================================================================

ColumnarReader::ColumnarReader(std::string path) {
    (void)open(std::move(path));
}

ColumnarReader::~ColumnarReader() {
    close();
}

ColumnarReader::ColumnarReader(ColumnarReader&& other) noexcept
    : path_(std::move(other.path_)),
      last_error_(std::move(other.last_error_)),
      last_status_(other.last_status_),
      mapped_base_(other.mapped_base_),
      pages_(other.pages_),
      page_offsets_(std::move(other.page_offsets_)),
      file_bytes_(other.file_bytes_),
      num_pages_(other.num_pages_),
      size_aligned_(other.size_aligned_),
#ifdef _WIN32
      file_handle_(other.file_handle_),
      mapping_handle_(other.mapping_handle_)
#else
      fd_(other.fd_)
#endif
{
    other.mapped_base_ = nullptr;
    other.pages_ = nullptr;
    other.file_bytes_ = 0;
    other.num_pages_ = 0;
    other.size_aligned_ = true;
#ifdef _WIN32
    other.file_handle_ = nullptr;
    other.mapping_handle_ = nullptr;
#else
    other.fd_ = -1;
#endif
}

ColumnarReader& ColumnarReader::operator=(ColumnarReader&& other) noexcept {
    if (this != &other) {
        close();
        path_ = std::move(other.path_);
        last_error_ = std::move(other.last_error_);
        last_status_ = other.last_status_;
        mapped_base_ = other.mapped_base_;
        pages_ = other.pages_;
        page_offsets_ = std::move(other.page_offsets_);
        file_bytes_ = other.file_bytes_;
        num_pages_ = other.num_pages_;
        size_aligned_ = other.size_aligned_;
#ifdef _WIN32
        file_handle_ = other.file_handle_;
        mapping_handle_ = other.mapping_handle_;
        other.file_handle_ = nullptr;
        other.mapping_handle_ = nullptr;
#else
        fd_ = other.fd_;
        other.fd_ = -1;
#endif
        other.mapped_base_ = nullptr;
        other.pages_ = nullptr;
        other.file_bytes_ = 0;
        other.num_pages_ = 0;
        other.size_aligned_ = true;
    }
    return *this;
}

bool ColumnarReader::open(std::string path) {
    close();
    path_ = std::move(path);
    last_error_.clear();
    last_status_ = RingStatus::Ok;

#ifdef _WIN32
    HANDLE fh = CreateFileA(path_.c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fh == INVALID_HANDLE_VALUE) {
        last_status_ = RingStatus::IoError;
        last_error_ = "CreateFileA failed for ColumnarReader";
        emit_log(LogLevel::Error, last_status_, "columnar_reader", last_error_);
        return false;
    }
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(fh, &sz)) {
        CloseHandle(fh);
        last_status_ = RingStatus::IoError;
        last_error_ = "GetFileSizeEx failed";
        emit_log(LogLevel::Error, last_status_, "columnar_reader", last_error_);
        return false;
    }
    file_bytes_ = static_cast<std::size_t>(sz.QuadPart);
    size_aligned_ = (file_bytes_ % sizeof(ColumnarPage)) == 0;
    num_pages_ = file_bytes_ / sizeof(ColumnarPage);
    if (file_bytes_ == 0) {
        CloseHandle(fh);
        return true;
    }
    HANDLE mh = CreateFileMappingA(fh, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (mh == nullptr) {
        CloseHandle(fh);
        last_status_ = RingStatus::MappingError;
        last_error_ = "CreateFileMappingA (READ) failed";
        emit_log(LogLevel::Error, last_status_, "columnar_reader", last_error_);
        return false;
    }
    void* view = MapViewOfFile(mh, FILE_MAP_READ, 0, 0, file_bytes_);
    if (view == nullptr) {
        CloseHandle(mh);
        CloseHandle(fh);
        last_status_ = RingStatus::MappingError;
        last_error_ = "MapViewOfFile (READ) failed";
        emit_log(LogLevel::Error, last_status_, "columnar_reader", last_error_);
        return false;
    }
    file_handle_ = fh;
    mapping_handle_ = mh;
    mapped_base_ = view;
    pages_ = static_cast<const ColumnarPage*>(view);
#else
    const int fd = ::open(path_.c_str(), O_RDONLY);
    if (fd < 0) {
        last_status_ = RingStatus::IoError;
        last_error_ = std::string("open failed for ColumnarReader: ") + std::strerror(errno);
        emit_log(LogLevel::Error, last_status_, "columnar_reader", last_error_);
        return false;
    }
    struct stat st{};
    if (::fstat(fd, &st) != 0) {
        ::close(fd);
        last_status_ = RingStatus::IoError;
        last_error_ = "fstat failed";
        emit_log(LogLevel::Error, last_status_, "columnar_reader", last_error_);
        return false;
    }
    file_bytes_ = static_cast<std::size_t>(st.st_size);
    if (file_bytes_ == 0) {
        ::close(fd);
        size_aligned_ = true;
        num_pages_ = 0;
        return true;
    }
    void* view = ::mmap(nullptr, file_bytes_, PROT_READ, MAP_PRIVATE, fd, 0);
    if (view == MAP_FAILED) {
        ::close(fd);
        last_status_ = RingStatus::MappingError;
        last_error_ = std::string("mmap (READ) failed: ") + std::strerror(errno);
        emit_log(LogLevel::Error, last_status_, "columnar_reader", last_error_);
        return false;
    }
    fd_ = fd;
    mapped_base_ = view;
    pages_ = static_cast<const ColumnarPage*>(view);
#endif

    page_offsets_.clear();
    size_aligned_ = true;
    const auto* base = static_cast<const std::uint8_t*>(mapped_base_);
    std::size_t offset = 0;
    while (offset < file_bytes_) {
        if (offset + sizeof(ColumnarPageHeader) > file_bytes_) {
            size_aligned_ = false;
            break;
        }
        ColumnarPageHeader hdr{};
        std::memcpy(&hdr, base + offset, sizeof(ColumnarPageHeader));
        if (hdr.magic != kColumnarPageMagic ||
            hdr.capacity != static_cast<std::uint32_t>(kColumnarPageEvents)) {
            size_aligned_ = false;
            break;
        }
        std::size_t page_bytes = sizeof(ColumnarPage);
        if ((hdr.flags & kPageFlagLz4) != 0u) {
            if (hdr.compressed_bytes == 0 ||
                hdr.compressed_bytes > (sizeof(ColumnarPage) - sizeof(ColumnarPageHeader))) {
                size_aligned_ = false;
                break;
            }
            page_bytes = sizeof(ColumnarPageHeader) + hdr.compressed_bytes;
        }
        if (offset + page_bytes > file_bytes_) {
            size_aligned_ = false;
            break;
        }
        page_offsets_.push_back(offset);
        offset += page_bytes;
    }
    num_pages_ = page_offsets_.size();
    return true;
}

void ColumnarReader::close() noexcept {
#ifdef _WIN32
    if (mapped_base_ != nullptr) {
        UnmapViewOfFile(mapped_base_);
        mapped_base_ = nullptr;
    }
    if (mapping_handle_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(mapping_handle_));
        mapping_handle_ = nullptr;
    }
    if (file_handle_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(file_handle_));
        file_handle_ = nullptr;
    }
#else
    if (mapped_base_ != nullptr) {
        ::munmap(mapped_base_, file_bytes_);
        mapped_base_ = nullptr;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
#endif
    pages_ = nullptr;
    page_offsets_.clear();
    file_bytes_ = 0;
    num_pages_ = 0;
    size_aligned_ = true;
}

bool ColumnarReader::verify_page_at(std::size_t index) const noexcept {
    if (index >= num_pages_ || mapped_base_ == nullptr) {
        return false;
    }
    const auto* base = static_cast<const std::uint8_t*>(mapped_base_) + page_offsets_[index];
    ColumnarPageHeader hdr{};
    std::memcpy(&hdr, base, sizeof(ColumnarPageHeader));
    if (hdr.magic != kColumnarPageMagic ||
        hdr.version != kColumnarFormatVersion ||
        hdr.capacity != static_cast<std::uint32_t>(kColumnarPageEvents) ||
        hdr.event_count > kColumnarPageEvents) {
        return false;
    }
    ColumnarPageHeader hdr_copy = hdr;
    hdr_copy.header_crc32c = 0;
    if (crc32c(&hdr_copy, sizeof(ColumnarPageHeader)) != hdr.header_crc32c) {
        return false;
    }
    const auto* payload = base + sizeof(ColumnarPageHeader);
    const std::size_t payload_bytes = ((hdr.flags & kPageFlagLz4) != 0u)
                                          ? static_cast<std::size_t>(hdr.compressed_bytes)
                                          : (sizeof(ColumnarPage) - sizeof(ColumnarPageHeader));
    return crc32c(payload, payload_bytes) == hdr.data_crc32c;
}

bool ColumnarReader::read_page(std::size_t index, ColumnarPage& out) const noexcept {
    if (!verify_page_at(index)) {
        return false;
    }
    const auto* base = static_cast<const std::uint8_t*>(mapped_base_) + page_offsets_[index];
    ColumnarPageHeader hdr{};
    std::memcpy(&hdr, base, sizeof(ColumnarPageHeader));
    if ((hdr.flags & kPageFlagLz4) == 0u) {
        std::memcpy(static_cast<void*>(&out), base, sizeof(ColumnarPage));
        return true;
    }
    out = ColumnarPage{};
    out.header = hdr;
    constexpr std::size_t kPayloadBytes = sizeof(ColumnarPage) - sizeof(ColumnarPageHeader);
    auto* dst_payload =
        reinterpret_cast<std::uint8_t*>(&out) + sizeof(ColumnarPageHeader);
    const std::size_t decomp = lz4_decompress(
        base + sizeof(ColumnarPageHeader), hdr.compressed_bytes, dst_payload, kPayloadBytes);
    return decomp == kPayloadBytes;
}

std::size_t ColumnarReader::total_events() const noexcept {
    std::size_t total = 0;
    for (std::size_t i = 0; i < num_pages_; ++i) {
        total += std::min<std::size_t>(page_header(i).event_count, kColumnarPageEvents);
    }
    return total;
}

bool ColumnarReader::verify_all_pages(std::size_t* corrupted_page_index) const noexcept {
    if (!size_aligned_) {
        if (corrupted_page_index != nullptr) {
            *corrupted_page_index = num_pages_;
        }
        emit_log(LogLevel::Error, RingStatus::CorruptedPage, "columnar_reader",
                 "Columnar file size or page framing is corrupted", file_bytes_);
        return false;
    }
    for (std::size_t i = 0; i < num_pages_; ++i) {
        if (!verify_page_at(i)) {
            if (corrupted_page_index != nullptr) {
                *corrupted_page_index = i;
            }
            emit_log(LogLevel::Error, RingStatus::CorruptedPage, "columnar_reader",
                     "CRC32C verification failed on page", i);
            return false;
        }
    }
    return true;
}

std::vector<PageIndexEntry> ColumnarReader::page_index() const {
    std::vector<PageIndexEntry> entries;
    entries.reserve(num_pages_);
    for (std::size_t i = 0; i < num_pages_; ++i) {
        const ColumnarPageHeader& h = page_header(i);
        entries.push_back(PageIndexEntry{
            i,
            h.event_count,
            h.min_timestamp_ns,
            h.max_timestamp_ns,
            h.first_sequence_id,
            h.last_sequence_id});
    }
    return entries;
}

std::vector<std::size_t> ColumnarReader::find_pages_by_timestamp(
    std::uint64_t min_ts, std::uint64_t max_ts) const {
    std::vector<std::size_t> matches;
    if (min_ts > max_ts) {
        return matches;
    }
    for (std::size_t i = 0; i < num_pages_; ++i) {
        const ColumnarPageHeader& h = page_header(i);
        if (h.event_count == 0 || h.magic != kColumnarPageMagic) {
            continue;
        }
        if (h.max_timestamp_ns >= min_ts && h.min_timestamp_ns <= max_ts) {
            matches.push_back(i);
        }
    }
    return matches;
}

Expected<std::size_t, RingStatus> ColumnarReader::find_page_by_sequence(
    std::uint64_t sequence_id) const noexcept {
    for (std::size_t i = 0; i < num_pages_; ++i) {
        const ColumnarPageHeader& h = page_header(i);
        if (h.event_count == 0 || h.magic != kColumnarPageMagic) {
            continue;
        }
        if (sequence_id >= h.first_sequence_id && sequence_id <= h.last_sequence_id) {
            return i;
        }
    }
    return unexpected(RingStatus::Empty);
}

Expected<std::size_t, RingStatus> ColumnarReader::replay_filtered(
    const TriggerConfig& cfg, SimdBackend backend,
    std::uint64_t* out_total_scanned) const noexcept {
    if (!is_open() && file_bytes_ > 0) {
        return unexpected(RingStatus::IoError);
    }
    if (!size_aligned_) {
        return unexpected(RingStatus::CorruptedPage);
    }
    std::size_t total_passed = 0;
    std::uint64_t total_scanned = 0;
    std::unique_ptr<ColumnarPage> decomp_scratch;
    for (std::size_t i = 0; i < num_pages_; ++i) {
        if (!verify_page_at(i)) {
            emit_log(LogLevel::Error, RingStatus::CorruptedPage, "columnar_reader",
                     "Replay aborted on corrupted page", i);
            return unexpected(RingStatus::CorruptedPage);
        }
        const ColumnarPageHeader& h = page_header(i);
        total_scanned += std::min<std::size_t>(h.event_count, kColumnarPageEvents);
        if ((h.flags & kPageFlagLz4) != 0u) {
            if (!decomp_scratch) {
                decomp_scratch = std::make_unique<ColumnarPage>();
            }
            if (!read_page(i, *decomp_scratch)) {
                return unexpected(RingStatus::CompressionError);
            }
            total_passed += evaluate_columnar_page(*decomp_scratch, cfg, nullptr, backend);
        } else {
            total_passed += evaluate_columnar_page(page(i), cfg, nullptr, backend);
        }
    }
    if (out_total_scanned != nullptr) {
        *out_total_scanned = total_scanned;
    }
    return total_passed;
}

} // namespace chronoring
