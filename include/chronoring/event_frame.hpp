#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace chronoring {

inline constexpr std::size_t kCacheLineSize = 64;
inline constexpr std::size_t kColumnarPageEvents = 1024;
inline constexpr std::uint32_t kColumnarPageMagic = 0x43524E47u; // "CRNG"
inline constexpr std::uint16_t kColumnarFormatVersion = 1u;

enum QualityFlags : std::uint32_t {
    kFlagNone       = 0u,
    kFlagValid      = 1u << 0,
    kFlagCalibrated = 1u << 1,
    kFlagSaturated  = 1u << 2,
    kFlagVeto       = 1u << 3,
    kFlagPileupWarn = 1u << 4
};

enum ColumnProjectionMask : std::uint16_t {
    kColTimestamps      = 1u << 0,
    kColDetectorChannel = 1u << 1,
    kColKinematics      = 1u << 2,
    kColPositions       = 1u << 3,
    kColQualityTrigger  = 1u << 4,
    kColAll             = 0x001Fu
};

struct alignas(kCacheLineSize) EventFrame {
    std::uint64_t timestamp_ns{0};
    std::uint64_t sequence_id{0};
    std::uint32_t detector_id{0};
    std::uint32_t channel_id{0};
    float energy_mev{0.0f};
    float timing_ps{0.0f};
    float pos_x_mm{0.0f};
    float pos_y_mm{0.0f};
    float pos_z_mm{0.0f};
    float pileup_score{0.0f};
    std::uint32_t quality_flags{0};
    std::uint16_t multiplicity{0};
    std::uint16_t trigger_mask{0};
    std::uint32_t payload_crc{0};
    std::uint32_t reserved{0};
};

static_assert(sizeof(EventFrame) == 64, "EventFrame must be exactly one 64-byte cache line");
static_assert(alignof(EventFrame) == 64, "EventFrame must be 64-byte aligned");
static_assert(std::is_trivially_copyable_v<EventFrame>, "EventFrame must be trivially copyable");

struct alignas(kCacheLineSize) ColumnarPageHeader {
    std::uint32_t magic{kColumnarPageMagic};
    std::uint16_t version{kColumnarFormatVersion};
    std::uint16_t flags{0};
    std::uint32_t event_count{0};
    std::uint32_t capacity{static_cast<std::uint32_t>(kColumnarPageEvents)};
    std::uint64_t min_timestamp_ns{0};
    std::uint64_t max_timestamp_ns{0};
    std::uint64_t first_sequence_id{0};
    std::uint64_t last_sequence_id{0};
    std::uint32_t data_crc32c{0};
    std::uint32_t header_crc32c{0};
    std::uint64_t reserved{0};
};

static_assert(sizeof(ColumnarPageHeader) == 64, "ColumnarPageHeader must be exactly 64 bytes");
static_assert(alignof(ColumnarPageHeader) == 64, "ColumnarPageHeader must be 64-byte aligned");
static_assert(std::is_trivially_copyable_v<ColumnarPageHeader>, "ColumnarPageHeader must be trivially copyable");

struct alignas(kCacheLineSize) ColumnarPage {
    ColumnarPageHeader header{};

    alignas(kCacheLineSize) std::array<std::uint64_t, kColumnarPageEvents> timestamp_ns{};
    alignas(kCacheLineSize) std::array<std::uint64_t, kColumnarPageEvents> sequence_id{};
    alignas(kCacheLineSize) std::array<std::uint32_t, kColumnarPageEvents> detector_id{};
    alignas(kCacheLineSize) std::array<std::uint32_t, kColumnarPageEvents> channel_id{};
    alignas(kCacheLineSize) std::array<float, kColumnarPageEvents> energy_mev{};
    alignas(kCacheLineSize) std::array<float, kColumnarPageEvents> timing_ps{};
    alignas(kCacheLineSize) std::array<float, kColumnarPageEvents> pos_x_mm{};
    alignas(kCacheLineSize) std::array<float, kColumnarPageEvents> pos_y_mm{};
    alignas(kCacheLineSize) std::array<float, kColumnarPageEvents> pos_z_mm{};
    alignas(kCacheLineSize) std::array<float, kColumnarPageEvents> pileup_score{};
    alignas(kCacheLineSize) std::array<std::uint32_t, kColumnarPageEvents> quality_flags{};
    alignas(kCacheLineSize) std::array<std::uint16_t, kColumnarPageEvents> multiplicity{};
    alignas(kCacheLineSize) std::array<std::uint16_t, kColumnarPageEvents> trigger_mask{};
};

static_assert(sizeof(ColumnarPage) == 57408, "ColumnarPage must have zero inter-column padding");
static_assert(alignof(ColumnarPage) == 64, "ColumnarPage must be 64-byte aligned");
static_assert(std::is_trivially_copyable_v<ColumnarPage>, "ColumnarPage must be trivially copyable");

} // namespace chronoring
