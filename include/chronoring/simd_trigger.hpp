#pragma once

#include "chronoring/event_frame.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace chronoring {

enum class SimdBackend {
    Auto,
    Scalar,
    SSE42,
    AVX2,
    NEON
};

enum RejectReason : std::uint8_t {
    kRejectNone           = 0u,
    kRejectMinEnergy      = 1u << 0,
    kRejectMaxEnergy      = 1u << 1,
    kRejectTiming         = 1u << 2,
    kRejectPileup         = 1u << 3,
    kRejectRequiredFlags  = 1u << 4,
    kRejectForbiddenFlags = 1u << 5,
    kRejectMultiplicity   = 1u << 6,
    kRejectTriggerMask    = 1u << 7
};

using CustomTriggerPredicate = bool (*)(const EventFrame& ev, void* user_ctx) noexcept;

struct TriggerConfig {
    float min_energy_mev{0.5f};
    float max_energy_mev{10000.0f};
    float max_abs_timing_ps{250.0f};
    float max_pileup_score{0.5f};
    std::uint32_t required_flags{kFlagValid};
    std::uint32_t forbidden_flags{kFlagSaturated | kFlagVeto};
    std::uint16_t min_multiplicity{1};
    std::uint16_t required_trigger_mask{0};
    CustomTriggerPredicate custom_predicate{nullptr};
    void* custom_user_ctx{nullptr};
};

[[nodiscard]] bool cpu_supports_avx2() noexcept;
[[nodiscard]] bool cpu_supports_sse42() noexcept;
[[nodiscard]] bool cpu_supports_neon() noexcept;

[[nodiscard]] SimdBackend active_backend(SimdBackend requested = SimdBackend::Auto) noexcept;
[[nodiscard]] const char* backend_name(SimdBackend backend) noexcept;

[[nodiscard]] inline bool evaluate_scalar_one(const EventFrame& ev,
                                              const TriggerConfig& cfg) noexcept {
    if (!(ev.energy_mev >= cfg.min_energy_mev && ev.energy_mev <= cfg.max_energy_mev)) {
        return false;
    }
    if (!(std::fabs(ev.timing_ps) <= cfg.max_abs_timing_ps)) {
        return false;
    }
    if (!(ev.pileup_score <= cfg.max_pileup_score)) {
        return false;
    }
    if ((ev.quality_flags & cfg.required_flags) != cfg.required_flags) {
        return false;
    }
    if ((ev.quality_flags & cfg.forbidden_flags) != 0u) {
        return false;
    }
    if (ev.multiplicity < cfg.min_multiplicity) {
        return false;
    }
    if (cfg.required_trigger_mask != 0u &&
        (ev.trigger_mask & cfg.required_trigger_mask) == 0u) {
        return false;
    }
    if (cfg.custom_predicate != nullptr &&
        !cfg.custom_predicate(ev, cfg.custom_user_ctx)) {
        return false;
    }
    return true;
}

[[nodiscard]] inline std::uint8_t diagnose_reject_reasons(
    const EventFrame& ev, const TriggerConfig& cfg) noexcept {
    std::uint8_t reasons = kRejectNone;
    if (!(ev.energy_mev >= cfg.min_energy_mev)) {
        reasons |= kRejectMinEnergy;
    }
    if (!(ev.energy_mev <= cfg.max_energy_mev)) {
        reasons |= kRejectMaxEnergy;
    }
    if (!(std::fabs(ev.timing_ps) <= cfg.max_abs_timing_ps)) {
        reasons |= kRejectTiming;
    }
    if (!(ev.pileup_score <= cfg.max_pileup_score)) {
        reasons |= kRejectPileup;
    }
    if ((ev.quality_flags & cfg.required_flags) != cfg.required_flags) {
        reasons |= kRejectRequiredFlags;
    }
    if ((ev.quality_flags & cfg.forbidden_flags) != 0u) {
        reasons |= kRejectForbiddenFlags;
    }
    if (ev.multiplicity < cfg.min_multiplicity) {
        reasons |= kRejectMultiplicity;
    }
    if (cfg.required_trigger_mask != 0u &&
        (ev.trigger_mask & cfg.required_trigger_mask) == 0u) {
        reasons |= kRejectTriggerMask;
    }
    return reasons;
}

std::size_t diagnose_batch_reject_reasons(const EventFrame* in, std::size_t count,
                                          const TriggerConfig& cfg,
                                          std::uint8_t* out_reasons) noexcept;

inline std::size_t diagnose_batch_reject_reasons(std::span<const EventFrame> in,
                                                 const TriggerConfig& cfg,
                                                 std::span<std::uint8_t> out_reasons) noexcept {
    const std::size_t n = std::min(in.size(), out_reasons.size());
    return diagnose_batch_reject_reasons(in.data(), n, cfg, out_reasons.data());
}

std::size_t filter_events_scalar(const EventFrame* in, std::size_t count,
                                 const TriggerConfig& cfg, EventFrame* out) noexcept;

std::size_t filter_events_sse42(const EventFrame* in, std::size_t count,
                                const TriggerConfig& cfg, EventFrame* out) noexcept;

std::size_t filter_events_avx2(const EventFrame* in, std::size_t count,
                               const TriggerConfig& cfg, EventFrame* out) noexcept;

std::size_t filter_events_neon(const EventFrame* in, std::size_t count,
                               const TriggerConfig& cfg, EventFrame* out) noexcept;

std::size_t filter_events(const EventFrame* in, std::size_t count,
                          const TriggerConfig& cfg, EventFrame* out,
                          SimdBackend backend = SimdBackend::Auto) noexcept;

inline std::size_t filter_events(std::span<const EventFrame> in,
                                 const TriggerConfig& cfg,
                                 std::span<EventFrame> out,
                                 SimdBackend backend = SimdBackend::Auto) noexcept {
    if (out.size() < in.size()) {
        return 0;
    }
    return filter_events(in.data(), in.size(), cfg, out.data(), backend);
}

std::size_t evaluate_batch_bitmask_scalar(const EventFrame* in, std::size_t count,
                                          const TriggerConfig& cfg,
                                          std::uint64_t* out_bitmask_words) noexcept;

std::size_t evaluate_batch_bitmask_sse42(const EventFrame* in, std::size_t count,
                                         const TriggerConfig& cfg,
                                         std::uint64_t* out_bitmask_words) noexcept;

std::size_t evaluate_batch_bitmask_avx2(const EventFrame* in, std::size_t count,
                                        const TriggerConfig& cfg,
                                        std::uint64_t* out_bitmask_words) noexcept;

std::size_t evaluate_batch_bitmask_neon(const EventFrame* in, std::size_t count,
                                        const TriggerConfig& cfg,
                                        std::uint64_t* out_bitmask_words) noexcept;

std::size_t evaluate_batch_bitmask(const EventFrame* in, std::size_t count,
                                   const TriggerConfig& cfg,
                                   std::uint64_t* out_bitmask_words,
                                   SimdBackend backend = SimdBackend::Auto) noexcept;

std::size_t evaluate_columnar_page_scalar(const ColumnarPage& page,
                                          const TriggerConfig& cfg,
                                          std::uint64_t* out_bitmask_words) noexcept;

std::size_t evaluate_columnar_page_sse42(const ColumnarPage& page,
                                         const TriggerConfig& cfg,
                                         std::uint64_t* out_bitmask_words) noexcept;

std::size_t evaluate_columnar_page_avx2(const ColumnarPage& page,
                                        const TriggerConfig& cfg,
                                        std::uint64_t* out_bitmask_words) noexcept;

std::size_t evaluate_columnar_page_neon(const ColumnarPage& page,
                                        const TriggerConfig& cfg,
                                        std::uint64_t* out_bitmask_words) noexcept;

std::size_t evaluate_columnar_page(const ColumnarPage& page,
                                   const TriggerConfig& cfg,
                                   std::uint64_t* out_bitmask_words,
                                   SimdBackend backend = SimdBackend::Auto) noexcept;

std::size_t compact_columnar_page_to_aos(const ColumnarPage& page,
                                         const std::uint64_t* bitmask_words,
                                         EventFrame* out) noexcept;

std::size_t filter_columnar_page_to_aos(const ColumnarPage& page,
                                        const TriggerConfig& cfg,
                                        EventFrame* out,
                                        SimdBackend backend = SimdBackend::Auto) noexcept;

[[nodiscard]] std::string to_json(const TriggerConfig& cfg);
[[nodiscard]] bool from_json(std::string_view json, TriggerConfig& out_cfg) noexcept;

} // namespace chronoring
