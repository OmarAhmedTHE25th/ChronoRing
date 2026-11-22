#ifndef CHRONORING_C89_COMPAT_H
#define CHRONORING_C89_COMPAT_H

/* Strict ISO C89 / ANSI C90 compatibility header for ChronoRing FFI. */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cr89_u64 {
    unsigned int lo;
    unsigned int hi;
} cr89_u64_t;

typedef struct cr89_event_frame {
    cr89_u64_t timestamp_ns;
    cr89_u64_t sequence_id;
    unsigned int detector_id;
    unsigned int channel_id;
    float energy_mev;
    float timing_ps;
    float pos_x_mm;
    float pos_y_mm;
    float pos_z_mm;
    float pileup_score;
    unsigned int quality_flags;
    unsigned short multiplicity;
    unsigned short trigger_mask;
    unsigned int payload_crc;
    unsigned int reserved;
} cr89_event_frame_t;

typedef struct cr89_trigger_config {
    float min_energy_mev;
    float max_energy_mev;
    float max_abs_timing_ps;
    float max_pileup_score;
    unsigned int required_flags;
    unsigned int forbidden_flags;
    unsigned short min_multiplicity;
    unsigned short required_trigger_mask;
} cr89_trigger_config_t;

typedef struct chronoring_spsc cr89_spsc_t;
typedef struct chronoring_sink cr89_sink_t;
typedef struct chronoring_reader cr89_reader_t;
typedef struct chronoring_shm cr89_shm_t;
typedef struct chronoring_pipeline cr89_pipeline_t;

const char* chronoring_version(void);
cr89_trigger_config_t chronoring_default_trigger_config(void);
size_t chronoring_filter_events(const cr89_event_frame_t* in_frames,
                                size_t count,
                                const cr89_trigger_config_t* cfg,
                                cr89_event_frame_t* out_frames);
unsigned int chronoring_crc32c(const void* data, size_t len, unsigned int initial);

cr89_spsc_t* chronoring_spsc_create(void);
void chronoring_spsc_destroy(cr89_spsc_t* ring);
int chronoring_spsc_try_push(cr89_spsc_t* ring, const cr89_event_frame_t* frame);
int chronoring_spsc_try_pop(cr89_spsc_t* ring, cr89_event_frame_t* out_frame);
size_t chronoring_spsc_size(const cr89_spsc_t* ring);

#ifdef __cplusplus
}
#endif

#endif /* CHRONORING_C89_COMPAT_H */
