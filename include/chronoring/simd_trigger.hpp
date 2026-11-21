#pragma once

#include "chronoring/event_frame.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace chronoring {

enum class SimdBackend {
    Auto,
    Scalar,
    SSE42,
    AVX2,
    NEON
};

struct TriggerConfig {
    float min_energy_mev{0.5f};
    float max_energy_mev{10000.0f};
    float max_abs_timing_ps{250.0f};
    float max_pileup_score{0.5f};
    std::uint32_t required_flags{kFlagValid};
    std::uint32_t forbidden_flags{kFlagSaturated | kFlagVeto};
    std::uint16_t min_multiplicity{1};
    std::uint16_t required_trigger_mask{0};
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
    return true;
}

std::size_t filter_events_scalar(const EventFrame* in, std::size_t count,
                                 const TriggerConfig& cfg, EventFrame* out) noexcept;

std::size_t filter_events_sse42(const EventFrame* in, std::size_t count,
                                const TriggerConfig& cfg, EventFrame* out) noexcept;

std::size_t filter_events_avx2(const EventFrame* in, std::size_t count,
                               const TriggerConfig& cfg, EventFrame* out) noexcept;

std::size_t filter_events(const EventFrame* in, std::size_t count,
                          const TriggerConfig& cfg, EventFrame* out,
                          SimdBackend backend = SimdBackend::Auto) noexcept;

std::size_t evaluate_batch_bitmask_scalar(const EventFrame* in, std::size_t count,
                                          const TriggerConfig& cfg,
                                          std::uint64_t* out_bitmask_words) noexcept;

std::size_t evaluate_batch_bitmask_sse42(const EventFrame* in, std::size_t count,
                                         const TriggerConfig& cfg,
                                         std::uint64_t* out_bitmask_words) noexcept;

std::size_t evaluate_batch_bitmask_avx2(const EventFrame* in, std::size_t count,
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

std::size_t evaluate_columnar_page(const ColumnarPage& page,
                                   const TriggerConfig& cfg,
                                   std::uint64_t* out_bitmask_words,
                                   SimdBackend backend = SimdBackend::Auto) noexcept;

} // namespace chronoring
