#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#if defined(CHRONORING_SHARED_BUILD)
#define CHRONORING_API __declspec(dllexport)
#elif defined(CHRONORING_SHARED_USE)
#define CHRONORING_API __declspec(dllimport)
#else
#define CHRONORING_API
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define CHRONORING_API __attribute__((visibility("default")))
#else
#define CHRONORING_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct chronoring_event_frame_t {
    uint64_t timestamp_ns;
    uint64_t sequence_id;
    uint32_t detector_id;
    uint32_t channel_id;
    float energy_mev;
    float timing_ps;
    float pos_x_mm;
    float pos_y_mm;
    float pos_z_mm;
    float pileup_score;
    uint32_t quality_flags;
    uint16_t multiplicity;
    uint16_t trigger_mask;
    uint32_t payload_crc;
    uint32_t reserved;
} chronoring_event_frame_t;

typedef struct chronoring_trigger_config_t {
    float min_energy_mev;
    float max_energy_mev;
    float max_abs_timing_ps;
    float max_pileup_score;
    uint32_t required_flags;
    uint32_t forbidden_flags;
    uint16_t min_multiplicity;
    uint16_t required_trigger_mask;
} chronoring_trigger_config_t;

typedef struct chronoring_spsc chronoring_spsc_t;

CHRONORING_API const char* chronoring_version(void);

CHRONORING_API chronoring_trigger_config_t chronoring_default_trigger_config(void);

CHRONORING_API size_t chronoring_filter_events(
    const chronoring_event_frame_t* in_frames,
    size_t count,
    const chronoring_trigger_config_t* cfg,
    chronoring_event_frame_t* out_frames);

CHRONORING_API uint32_t chronoring_crc32c(
    const void* data,
    size_t len,
    uint32_t initial);

CHRONORING_API chronoring_spsc_t* chronoring_spsc_create(void);
CHRONORING_API void chronoring_spsc_destroy(chronoring_spsc_t* ring);
CHRONORING_API int chronoring_spsc_try_push(
    chronoring_spsc_t* ring,
    const chronoring_event_frame_t* frame);
CHRONORING_API int chronoring_spsc_try_pop(
    chronoring_spsc_t* ring,
    chronoring_event_frame_t* out_frame);
CHRONORING_API size_t chronoring_spsc_size(const chronoring_spsc_t* ring);

#ifdef __cplusplus
}
#endif
