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
typedef struct chronoring_sink chronoring_sink_t;
typedef struct chronoring_reader chronoring_reader_t;
typedef struct chronoring_shm chronoring_shm_t;
typedef struct chronoring_pipeline chronoring_pipeline_t;

typedef struct chronoring_pipeline_config_t {
    const char* output_path;
    const char* secondary_output_path;
    chronoring_trigger_config_t trigger;
    int consumer_cpu_core;
    size_t batch_size;
    size_t initial_page_capacity;
    uint16_t projection_mask;
    uint8_t use_soa_staging;
    uint8_t enable_compression;
    uint8_t adaptive_batching;
    uint8_t reserved_pad;
} chronoring_pipeline_config_t;

typedef struct chronoring_pipeline_stats_t {
    uint64_t submitted_events;
    uint64_t dropped_events;
    uint64_t consumed_events;
    uint64_t passed_events;
    uint64_t rejected_events;
    uint64_t reject_reasons[5];
    uint64_t pages_written;
    uint64_t sink_errors;
    uint64_t backpressure_waits;
    double ring_fill_ratio;
    double elapsed_seconds;
    double throughput_meps;
    uint64_t min_ns;
    uint64_t p50_ns;
    uint64_t p90_ns;
    uint64_t p99_ns;
    uint64_t p99_9_ns;
    uint64_t max_ns;
    double mean_ns;
    uint8_t last_status;
    uint8_t backpressured;
    uint8_t reserved[6];
} chronoring_pipeline_stats_t;

CHRONORING_API const char* chronoring_version(void);
CHRONORING_API const char* chronoring_status_string(int status);

CHRONORING_API chronoring_trigger_config_t chronoring_default_trigger_config(void);
CHRONORING_API chronoring_pipeline_config_t chronoring_default_pipeline_config(void);

CHRONORING_API size_t chronoring_filter_events(
    const chronoring_event_frame_t* in_frames,
    size_t count,
    const chronoring_trigger_config_t* cfg,
    chronoring_event_frame_t* out_frames);

CHRONORING_API uint32_t chronoring_crc32c(
    const void* data,
    size_t len,
    uint32_t initial);

/* SPSC Ring */
CHRONORING_API chronoring_spsc_t* chronoring_spsc_create(void);
CHRONORING_API void chronoring_spsc_destroy(chronoring_spsc_t* ring);
CHRONORING_API int chronoring_spsc_try_push(
    chronoring_spsc_t* ring,
    const chronoring_event_frame_t* frame);
CHRONORING_API int chronoring_spsc_try_pop(
    chronoring_spsc_t* ring,
    chronoring_event_frame_t* out_frame);
CHRONORING_API size_t chronoring_spsc_size(const chronoring_spsc_t* ring);

/* ColumnarSink & ColumnarReader */
CHRONORING_API chronoring_sink_t* chronoring_sink_create(void);
CHRONORING_API int chronoring_sink_open(
    chronoring_sink_t* sink,
    const char* path,
    size_t initial_page_capacity,
    uint16_t projection_mask,
    int enable_compression);
CHRONORING_API size_t chronoring_sink_write_batch(
    chronoring_sink_t* sink,
    const chronoring_event_frame_t* frames,
    size_t count);
CHRONORING_API int chronoring_sink_flush(chronoring_sink_t* sink);
CHRONORING_API void chronoring_sink_close(chronoring_sink_t* sink);
CHRONORING_API void chronoring_sink_destroy(chronoring_sink_t* sink);
CHRONORING_API size_t chronoring_sink_pages_written(const chronoring_sink_t* sink);
CHRONORING_API size_t chronoring_sink_events_written(const chronoring_sink_t* sink);
CHRONORING_API size_t chronoring_sink_bytes_written(const chronoring_sink_t* sink);
CHRONORING_API int chronoring_sink_last_status(const chronoring_sink_t* sink);

CHRONORING_API chronoring_reader_t* chronoring_reader_open(const char* path);
CHRONORING_API void chronoring_reader_destroy(chronoring_reader_t* reader);
CHRONORING_API size_t chronoring_reader_num_pages(const chronoring_reader_t* reader);
CHRONORING_API size_t chronoring_reader_total_events(const chronoring_reader_t* reader);
CHRONORING_API int chronoring_reader_verify_all(
    const chronoring_reader_t* reader,
    size_t* out_corrupted_page);
CHRONORING_API int64_t chronoring_reader_replay_filtered(
    const chronoring_reader_t* reader,
    const chronoring_trigger_config_t* cfg,
    uint64_t* out_total_scanned);

/* Shared Memory Ring */
CHRONORING_API chronoring_shm_t* chronoring_shm_create(
    const char* name,
    size_t capacity,
    int unlink_on_destroy);
CHRONORING_API chronoring_shm_t* chronoring_shm_open(const char* name);
CHRONORING_API int chronoring_shm_try_push(
    chronoring_shm_t* shm,
    const chronoring_event_frame_t* frame);
CHRONORING_API int chronoring_shm_try_pop(
    chronoring_shm_t* shm,
    chronoring_event_frame_t* out_frame);
CHRONORING_API int chronoring_shm_is_closed(const chronoring_shm_t* shm);
CHRONORING_API size_t chronoring_shm_size(const chronoring_shm_t* shm);
CHRONORING_API void chronoring_shm_close(chronoring_shm_t* shm);
CHRONORING_API int chronoring_shm_unlink(const char* name);
CHRONORING_API void chronoring_shm_destroy(chronoring_shm_t* shm);

/* TelemetryPipeline */
CHRONORING_API chronoring_pipeline_t* chronoring_pipeline_create(
    const chronoring_pipeline_config_t* cfg);
CHRONORING_API int chronoring_pipeline_start(chronoring_pipeline_t* pipeline);
CHRONORING_API int chronoring_pipeline_submit(
    chronoring_pipeline_t* pipeline,
    const chronoring_event_frame_t* frame);
CHRONORING_API int chronoring_pipeline_submit_wait(
    chronoring_pipeline_t* pipeline,
    const chronoring_event_frame_t* frame);
CHRONORING_API size_t chronoring_pipeline_submit_batch(
    chronoring_pipeline_t* pipeline,
    const chronoring_event_frame_t* frames,
    size_t count);
CHRONORING_API int chronoring_pipeline_is_backpressured(
    const chronoring_pipeline_t* pipeline);
CHRONORING_API double chronoring_pipeline_ring_fill_ratio(
    const chronoring_pipeline_t* pipeline);
CHRONORING_API int chronoring_pipeline_last_status(
    const chronoring_pipeline_t* pipeline);
CHRONORING_API void chronoring_pipeline_stop(chronoring_pipeline_t* pipeline);
CHRONORING_API void chronoring_pipeline_stats(
    const chronoring_pipeline_t* pipeline,
    chronoring_pipeline_stats_t* out_stats);
CHRONORING_API void chronoring_pipeline_destroy(chronoring_pipeline_t* pipeline);

#ifdef __cplusplus
}
#endif
