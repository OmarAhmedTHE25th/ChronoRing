#include "chronoring/c_api.h"
#include "chronoring/columnar_sink.hpp"
#include "chronoring/event_frame.hpp"
#include "chronoring/simd_trigger.hpp"
#include "chronoring/spsc_ring.hpp"
#include "chronoring/status.hpp"

#include <cstring>
#include <new>
#include <type_traits>

static_assert(sizeof(chronoring_event_frame_t) == sizeof(chronoring::EventFrame),
              "C API EventFrame layout must match C++ EventFrame");
static_assert(std::is_trivially_copyable_v<chronoring_event_frame_t>);

struct chronoring_spsc {
    chronoring::SpscRingBuffer<chronoring::EventFrame, 4096> ring;
};

extern "C" {

const char* chronoring_version(void) {
    return CHRONORING_VERSION_STRING;
}

chronoring_trigger_config_t chronoring_default_trigger_config(void) {
    const chronoring::TriggerConfig def{};
    chronoring_trigger_config_t c{};
    c.min_energy_mev = def.min_energy_mev;
    c.max_energy_mev = def.max_energy_mev;
    c.max_abs_timing_ps = def.max_abs_timing_ps;
    c.max_pileup_score = def.max_pileup_score;
    c.required_flags = def.required_flags;
    c.forbidden_flags = def.forbidden_flags;
    c.min_multiplicity = def.min_multiplicity;
    c.required_trigger_mask = def.required_trigger_mask;
    return c;
}

size_t chronoring_filter_events(const chronoring_event_frame_t* in_frames,
                                size_t count,
                                const chronoring_trigger_config_t* cfg,
                                chronoring_event_frame_t* out_frames) {
    if (in_frames == nullptr || count == 0) {
        return 0;
    }
    chronoring::TriggerConfig cpp_cfg{};
    if (cfg != nullptr) {
        cpp_cfg.min_energy_mev = cfg->min_energy_mev;
        cpp_cfg.max_energy_mev = cfg->max_energy_mev;
        cpp_cfg.max_abs_timing_ps = cfg->max_abs_timing_ps;
        cpp_cfg.max_pileup_score = cfg->max_pileup_score;
        cpp_cfg.required_flags = cfg->required_flags;
        cpp_cfg.forbidden_flags = cfg->forbidden_flags;
        cpp_cfg.min_multiplicity = cfg->min_multiplicity;
        cpp_cfg.required_trigger_mask = cfg->required_trigger_mask;
    }
    // EventFrame has alignas(64) whereas C struct is standard 8-byte aligned;
    // copy through aligned stack chunks if caller buffer isn't 64B aligned.
    constexpr size_t kChunk = 256;
    chronoring::EventFrame in_buf[kChunk]{};
    chronoring::EventFrame out_buf[kChunk]{};
    size_t total_passed = 0;
    size_t offset = 0;

    while (offset < count) {
        const size_t n = (count - offset < kChunk) ? (count - offset) : kChunk;
        std::memcpy(static_cast<void*>(in_buf), in_frames + offset,
                    n * sizeof(chronoring::EventFrame));
        const size_t kept = chronoring::filter_events(
            in_buf, n, cpp_cfg, (out_frames != nullptr) ? out_buf : nullptr);
        if (out_frames != nullptr && kept > 0) {
            std::memcpy(out_frames + total_passed, out_buf,
                        kept * sizeof(chronoring::EventFrame));
        }
        total_passed += kept;
        offset += n;
    }
    return total_passed;
}

uint32_t chronoring_crc32c(const void* data, size_t len, uint32_t initial) {
    return chronoring::crc32c(data, len, initial);
}

chronoring_spsc_t* chronoring_spsc_create(void) {
    return new (std::nothrow) chronoring_spsc();
}

void chronoring_spsc_destroy(chronoring_spsc_t* ring) {
    delete ring;
}

int chronoring_spsc_try_push(chronoring_spsc_t* ring,
                             const chronoring_event_frame_t* frame) {
    if (ring == nullptr || frame == nullptr) {
        return 0;
    }
    chronoring::EventFrame aligned{};
    std::memcpy(static_cast<void*>(&aligned), frame, sizeof(chronoring::EventFrame));
    return ring->ring.try_push(aligned) ? 1 : 0;
}

int chronoring_spsc_try_pop(chronoring_spsc_t* ring,
                            chronoring_event_frame_t* out_frame) {
    if (ring == nullptr || out_frame == nullptr) {
        return 0;
    }
    chronoring::EventFrame aligned{};
    if (!ring->ring.try_pop(aligned)) {
        return 0;
    }
    std::memcpy(out_frame, &aligned, sizeof(chronoring::EventFrame));
    return 1;
}

size_t chronoring_spsc_size(const chronoring_spsc_t* ring) {
    if (ring == nullptr) {
        return 0;
    }
    return ring->ring.size_approx();
}

} // extern "C"
