#include "chronoring/c_api.h"
#include "chronoring/columnar_sink.hpp"
#include "chronoring/event_frame.hpp"
#include "chronoring/pipeline.hpp"
#include "chronoring/shm_ring.hpp"
#include "chronoring/simd_trigger.hpp"
#include "chronoring/spsc_ring.hpp"
#include "chronoring/status.hpp"

#include <cstring>
#include <new>
#include <string>
#include <type_traits>

static_assert(sizeof(chronoring_event_frame_t) == sizeof(chronoring::EventFrame),
              "C API EventFrame layout must match C++ EventFrame");
static_assert(std::is_trivially_copyable_v<chronoring_event_frame_t>);

namespace {

chronoring::TriggerConfig to_cpp_trigger(const chronoring_trigger_config_t* cfg) noexcept {
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
    return cpp_cfg;
}

} // namespace

struct chronoring_spsc {
    chronoring::SpscRingBuffer<chronoring::EventFrame, 4096> ring;
};

struct chronoring_sink {
    chronoring::ColumnarSink sink;
};

struct chronoring_reader {
    chronoring::ColumnarReader reader;
};

struct chronoring_shm {
    chronoring::ShmRingBuffer shm;
};

struct chronoring_pipeline {
    explicit chronoring_pipeline(chronoring::PipelineConfig cfg)
        : pipeline(std::move(cfg)) {}
    chronoring::TelemetryPipeline pipeline;
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

chronoring_pipeline_config_t chronoring_default_pipeline_config(void) {
    chronoring_pipeline_config_t cfg{};
    cfg.output_path = nullptr;
    cfg.trigger = chronoring_default_trigger_config();
    cfg.consumer_cpu_core = -1;
    cfg.batch_size = 256;
    cfg.initial_page_capacity = 64;
    cfg.projection_mask = chronoring::kColAll;
    cfg.use_soa_staging = 1;
    cfg.enable_compression = 0;
    cfg.adaptive_batching = 1;
    return cfg;
}

/* ColumnarSink & ColumnarReader */

chronoring_sink_t* chronoring_sink_create(void) {
    return new (std::nothrow) chronoring_sink();
}

int chronoring_sink_open(chronoring_sink_t* sink,
                         const char* path,
                         size_t initial_page_capacity,
                         uint16_t projection_mask,
                         int enable_compression) {
    if (sink == nullptr || path == nullptr) {
        return 0;
    }
    return sink->sink.open(path, initial_page_capacity, projection_mask,
                           enable_compression != 0)
               ? 1
               : 0;
}

size_t chronoring_sink_write_batch(chronoring_sink_t* sink,
                                   const chronoring_event_frame_t* frames,
                                   size_t count) {
    if (sink == nullptr || frames == nullptr || count == 0) {
        return 0;
    }
    constexpr size_t kChunk = 256;
    chronoring::EventFrame buf[kChunk]{};
    size_t total_written = 0;
    size_t offset = 0;
    while (offset < count) {
        const size_t n = (count - offset < kChunk) ? (count - offset) : kChunk;
        std::memcpy(static_cast<void*>(buf), frames + offset,
                    n * sizeof(chronoring::EventFrame));
        const size_t wrote = sink->sink.write_batch(buf, n);
        total_written += wrote;
        if (wrote < n) {
            break;
        }
        offset += n;
    }
    return total_written;
}

int chronoring_sink_flush(chronoring_sink_t* sink) {
    if (sink == nullptr) {
        return 0;
    }
    return sink->sink.flush() ? 1 : 0;
}

void chronoring_sink_close(chronoring_sink_t* sink) {
    if (sink != nullptr) {
        sink->sink.close();
    }
}

void chronoring_sink_destroy(chronoring_sink_t* sink) {
    delete sink;
}

size_t chronoring_sink_pages_written(const chronoring_sink_t* sink) {
    return (sink != nullptr) ? sink->sink.pages_written() : 0;
}

size_t chronoring_sink_events_written(const chronoring_sink_t* sink) {
    return (sink != nullptr) ? sink->sink.events_written() : 0;
}

int chronoring_sink_last_status(const chronoring_sink_t* sink) {
    return (sink != nullptr) ? static_cast<int>(sink->sink.last_status())
                             : static_cast<int>(chronoring::RingStatus::InvalidArgument);
}

chronoring_reader_t* chronoring_reader_open(const char* path) {
    if (path == nullptr) {
        return nullptr;
    }
    auto* r = new (std::nothrow) chronoring_reader();
    if (r == nullptr) {
        return nullptr;
    }
    if (!r->reader.open(path)) {
        delete r;
        return nullptr;
    }
    return r;
}

void chronoring_reader_destroy(chronoring_reader_t* reader) {
    delete reader;
}

size_t chronoring_reader_num_pages(const chronoring_reader_t* reader) {
    return (reader != nullptr) ? reader->reader.num_pages() : 0;
}

size_t chronoring_reader_total_events(const chronoring_reader_t* reader) {
    return (reader != nullptr) ? reader->reader.total_events() : 0;
}

int chronoring_reader_verify_all(const chronoring_reader_t* reader,
                                 size_t* out_corrupted_page) {
    if (reader == nullptr) {
        return 0;
    }
    return reader->reader.verify_all_pages(out_corrupted_page) ? 1 : 0;
}

int64_t chronoring_reader_replay_filtered(const chronoring_reader_t* reader,
                                          const chronoring_trigger_config_t* cfg,
                                          uint64_t* out_total_scanned) {
    if (reader == nullptr) {
        return -1;
    }
    const auto res = reader->reader.replay_filtered(
        to_cpp_trigger(cfg), chronoring::SimdBackend::Auto, out_total_scanned);
    if (!res.has_value()) {
        return -1;
    }
    return static_cast<int64_t>(*res);
}

/* Shared Memory Ring */

chronoring_shm_t* chronoring_shm_create(const char* name,
                                        size_t capacity,
                                        int unlink_on_destroy) {
    if (name == nullptr) {
        return nullptr;
    }
    auto* s = new (std::nothrow) chronoring_shm{
        chronoring::ShmRingBuffer(name, chronoring::ShmMode::Create, capacity,
                                  unlink_on_destroy != 0)};
    if (s == nullptr) {
        return nullptr;
    }
    if (!s->shm.is_open()) {
        delete s;
        return nullptr;
    }
    return s;
}

chronoring_shm_t* chronoring_shm_open(const char* name) {
    if (name == nullptr) {
        return nullptr;
    }
    auto* s = new (std::nothrow) chronoring_shm{
        chronoring::ShmRingBuffer(name, chronoring::ShmMode::Open)};
    if (s == nullptr) {
        return nullptr;
    }
    if (!s->shm.is_open()) {
        delete s;
        return nullptr;
    }
    return s;
}

int chronoring_shm_try_push(chronoring_shm_t* shm,
                            const chronoring_event_frame_t* frame) {
    if (shm == nullptr || frame == nullptr) {
        return 0;
    }
    chronoring::EventFrame aligned{};
    std::memcpy(static_cast<void*>(&aligned), frame, sizeof(chronoring::EventFrame));
    return shm->shm.try_push(aligned) ? 1 : 0;
}

int chronoring_shm_try_pop(chronoring_shm_t* shm,
                           chronoring_event_frame_t* out_frame) {
    if (shm == nullptr || out_frame == nullptr) {
        return 0;
    }
    chronoring::EventFrame aligned{};
    if (!shm->shm.try_pop(aligned)) {
        return 0;
    }
    std::memcpy(out_frame, &aligned, sizeof(chronoring::EventFrame));
    return 1;
}

int chronoring_shm_is_closed(const chronoring_shm_t* shm) {
    return (shm != nullptr && shm->shm.is_closed()) ? 1 : 0;
}

size_t chronoring_shm_size(const chronoring_shm_t* shm) {
    return (shm != nullptr) ? shm->shm.size_approx() : 0;
}

void chronoring_shm_close(chronoring_shm_t* shm) {
    if (shm != nullptr) {
        shm->shm.close();
    }
}

int chronoring_shm_unlink(const char* name) {
    if (name == nullptr) {
        return 0;
    }
    return chronoring::ShmRingBuffer::unlink(name) ? 1 : 0;
}

void chronoring_shm_destroy(chronoring_shm_t* shm) {
    delete shm;
}

/* TelemetryPipeline */

chronoring_pipeline_t* chronoring_pipeline_create(
    const chronoring_pipeline_config_t* cfg) {
    chronoring::PipelineConfig cpp_cfg{};
    if (cfg != nullptr) {
        if (cfg->output_path != nullptr) {
            cpp_cfg.output_path = cfg->output_path;
        }
        cpp_cfg.trigger = to_cpp_trigger(&cfg->trigger);
        cpp_cfg.consumer_cpu_core = cfg->consumer_cpu_core;
        if (cfg->batch_size > 0) {
            cpp_cfg.batch_size = cfg->batch_size;
        }
        if (cfg->initial_page_capacity > 0) {
            cpp_cfg.initial_page_capacity = cfg->initial_page_capacity;
        }
        if (cfg->projection_mask != 0) {
            cpp_cfg.projection_mask = cfg->projection_mask;
        }
        cpp_cfg.use_soa_staging = cfg->use_soa_staging != 0;
        cpp_cfg.enable_compression = cfg->enable_compression != 0;
        cpp_cfg.adaptive_batching = cfg->adaptive_batching != 0;
    }
    return new (std::nothrow) chronoring_pipeline(std::move(cpp_cfg));
}

int chronoring_pipeline_start(chronoring_pipeline_t* pipeline) {
    if (pipeline == nullptr) {
        return 0;
    }
    return pipeline->pipeline.start() ? 1 : 0;
}

int chronoring_pipeline_submit(chronoring_pipeline_t* pipeline,
                               const chronoring_event_frame_t* frame) {
    if (pipeline == nullptr || frame == nullptr) {
        return 0;
    }
    chronoring::EventFrame aligned{};
    std::memcpy(static_cast<void*>(&aligned), frame, sizeof(chronoring::EventFrame));
    return pipeline->pipeline.submit(aligned) ? 1 : 0;
}

size_t chronoring_pipeline_submit_batch(chronoring_pipeline_t* pipeline,
                                        const chronoring_event_frame_t* frames,
                                        size_t count) {
    if (pipeline == nullptr || frames == nullptr || count == 0) {
        return 0;
    }
    constexpr size_t kChunk = 256;
    chronoring::EventFrame buf[kChunk]{};
    size_t pushed = 0;
    size_t offset = 0;
    while (offset < count) {
        const size_t n = (count - offset < kChunk) ? (count - offset) : kChunk;
        std::memcpy(static_cast<void*>(buf), frames + offset,
                    n * sizeof(chronoring::EventFrame));
        pushed += pipeline->pipeline.submit_batch(buf, n);
        offset += n;
    }
    return pushed;
}

void chronoring_pipeline_stop(chronoring_pipeline_t* pipeline) {
    if (pipeline != nullptr) {
        pipeline->pipeline.stop();
    }
}

void chronoring_pipeline_stats(const chronoring_pipeline_t* pipeline,
                               chronoring_pipeline_stats_t* out_stats) {
    if (pipeline == nullptr || out_stats == nullptr) {
        return;
    }
    const chronoring::PipelineStats s = pipeline->pipeline.stats();
    out_stats->submitted_events = s.submitted_events;
    out_stats->dropped_events = s.dropped_events;
    out_stats->consumed_events = s.consumed_events;
    out_stats->passed_events = s.passed_events;
    out_stats->rejected_events = s.rejected_events;
    for (size_t i = 0; i < 5; ++i) {
        out_stats->reject_reasons[i] = s.reject_reasons[i];
    }
    out_stats->pages_written = s.pages_written;
    out_stats->sink_errors = s.sink_errors;
    out_stats->backpressure_waits = s.backpressure_waits;
    out_stats->ring_fill_ratio = s.ring_fill_ratio;
    out_stats->elapsed_seconds = s.elapsed_seconds;
    out_stats->throughput_meps = s.throughput_meps;
    out_stats->min_ns = s.min_ns;
    out_stats->p50_ns = s.p50_ns;
    out_stats->p90_ns = s.p90_ns;
    out_stats->p99_ns = s.p99_ns;
    out_stats->p99_9_ns = s.p99_9_ns;
    out_stats->max_ns = s.max_ns;
    out_stats->mean_ns = s.mean_ns;
    out_stats->last_status = static_cast<uint8_t>(s.last_status);
    out_stats->backpressured = s.backpressured ? 1 : 0;
}

void chronoring_pipeline_destroy(chronoring_pipeline_t* pipeline) {
    delete pipeline;
}

} // extern "C"
