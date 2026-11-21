#pragma once

#include "chronoring/columnar_sink.hpp"
#include "chronoring/event_frame.hpp"
#include "chronoring/latency_histogram.hpp"
#include "chronoring/simd_trigger.hpp"
#include "chronoring/spsc_ring.hpp"
#include "chronoring/status.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace chronoring {

struct PipelineConfig {
    std::string output_path{};
    TriggerConfig trigger{};
    SimdBackend simd_backend{SimdBackend::Auto};
    int consumer_cpu_core{-1};
    std::size_t batch_size{256};
    std::size_t initial_page_capacity{64};
    std::uint16_t projection_mask{kColAll};
    bool use_soa_staging{true};
};

struct PipelineStats {
    std::uint64_t submitted_events{0};
    std::uint64_t dropped_events{0};
    std::uint64_t consumed_events{0};
    std::uint64_t passed_events{0};
    std::uint64_t rejected_events{0};
    std::uint64_t pages_written{0};
    std::uint64_t sink_errors{0};
    std::uint64_t backpressure_waits{0};
    RingStatus last_status{RingStatus::Ok};
    double elapsed_seconds{0.0};
    double throughput_meps{0.0};
    std::uint64_t min_ns{0};
    std::uint64_t p50_ns{0};
    std::uint64_t p90_ns{0};
    std::uint64_t p99_ns{0};
    std::uint64_t p99_9_ns{0};
    std::uint64_t max_ns{0};
    double mean_ns{0.0};
};

inline bool pin_current_thread_to_core(int core_id) noexcept {
    if (core_id < 0) {
        return false;
    }
#ifdef _WIN32
    if (core_id >= 64) {
        return false;
    }
    const DWORD_PTR mask = DWORD_PTR{1} << static_cast<unsigned>(core_id);
    return SetThreadAffinityMask(GetCurrentThread(), mask) != 0;
#elif defined(__linux__)
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    if (core_id >= CPU_SETSIZE) {
        return false;
    }
    CPU_SET(core_id, &cpuset);
    return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) == 0;
#else
    (void)core_id;
    return false;
#endif
}

class TelemetryPipeline {
public:
    static constexpr std::size_t kRingCapacity = 16384;

    explicit TelemetryPipeline(PipelineConfig config)
        : config_(std::move(config)),
          ring_(std::make_unique<SpscRingBuffer<StampedFrame, kRingCapacity>>()),
          soa_staging_(std::make_unique<ColumnarPage>()) {
        const std::size_t bsz = std::max<std::size_t>(1, config_.batch_size);
        batch_stamped_.resize(bsz);
        batch_frames_.resize(bsz);
        batch_filtered_.resize(bsz);
    }

    ~TelemetryPipeline() {
        stop();
    }

    TelemetryPipeline(const TelemetryPipeline&) = delete;
    TelemetryPipeline& operator=(const TelemetryPipeline&) = delete;

    [[nodiscard]] bool start() {
        if (running_.load(std::memory_order_acquire)) {
            return false;
        }
        last_status_.store(RingStatus::Ok, std::memory_order_relaxed);
        if (!config_.output_path.empty()) {
            if (!sink_.open(config_.output_path, config_.initial_page_capacity,
                            config_.projection_mask)) {
                last_status_.store(sink_.last_status(), std::memory_order_release);
                emit_log(LogLevel::Error, sink_.last_status(), "pipeline",
                         sink_.last_error());
                return false;
            }
        }
        histogram_.reset();
        submitted_.store(0, std::memory_order_relaxed);
        dropped_.store(0, std::memory_order_relaxed);
        consumed_.store(0, std::memory_order_relaxed);
        passed_.store(0, std::memory_order_relaxed);
        rejected_.store(0, std::memory_order_relaxed);
        pages_written_.store(0, std::memory_order_relaxed);
        sink_errors_.store(0, std::memory_order_relaxed);
        backpressure_waits_.store(0, std::memory_order_relaxed);
        stop_requested_.store(false, std::memory_order_release);
        running_.store(true, std::memory_order_release);
        start_time_ = std::chrono::steady_clock::now();

        consumer_thread_ = std::thread([this]() { consumer_loop(); });
        return true;
    }

    [[nodiscard]] bool submit(const EventFrame& frame) noexcept {
        const std::uint64_t now_ns = monotonic_now_ns();
        const StampedFrame sf{frame, now_ns};
        if (ring_->try_push(sf)) {
            submitted_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        dropped_.fetch_add(1, std::memory_order_relaxed);
        emit_log(LogLevel::Warn, RingStatus::Full, "pipeline",
                 "Ingestion ring full; frame dropped", frame.sequence_id);
        return false;
    }

    void submit_wait(const EventFrame& frame) noexcept {
        const std::uint64_t now_ns = monotonic_now_ns();
        const StampedFrame sf{frame, now_ns};
        if (!ring_->try_push(sf)) {
            backpressure_waits_.fetch_add(1, std::memory_order_relaxed);
            while (!ring_->try_push(sf)) {
                if (stop_requested_.load(std::memory_order_acquire)) {
                    return;
                }
                std::this_thread::yield();
            }
        }
        submitted_.fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t submit_batch(const EventFrame* frames,
                                           std::size_t count) noexcept {
        if (frames == nullptr || count == 0) {
            return 0;
        }
        std::size_t pushed = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (submit(frames[i])) {
                ++pushed;
            }
        }
        return pushed;
    }

    void stop() {
        if (!running_.exchange(false, std::memory_order_acq_rel)) {
            if (consumer_thread_.joinable()) {
                consumer_thread_.join();
            }
            return;
        }
        stop_requested_.store(true, std::memory_order_release);
        if (consumer_thread_.joinable()) {
            consumer_thread_.join();
        }
        end_time_ = std::chrono::steady_clock::now();
        if (sink_.is_open()) {
            sink_.close();
            pages_written_.store(sink_.pages_written(), std::memory_order_relaxed);
            if (sink_.last_status() != RingStatus::Ok) {
                last_status_.store(sink_.last_status(), std::memory_order_release);
                sink_errors_.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    [[nodiscard]] PipelineStats stats() const noexcept {
        const auto end = running_.load(std::memory_order_acquire)
                             ? std::chrono::steady_clock::now()
                             : end_time_;
        const double secs =
            std::chrono::duration<double>(end - start_time_).count();

        PipelineStats s;
        s.submitted_events = submitted_.load(std::memory_order_relaxed);
        s.dropped_events = dropped_.load(std::memory_order_relaxed);
        s.consumed_events = consumed_.load(std::memory_order_relaxed);
        s.passed_events = passed_.load(std::memory_order_relaxed);
        s.rejected_events = rejected_.load(std::memory_order_relaxed);
        s.pages_written = pages_written_.load(std::memory_order_relaxed);
        s.sink_errors = sink_errors_.load(std::memory_order_relaxed);
        s.backpressure_waits = backpressure_waits_.load(std::memory_order_relaxed);
        s.last_status = last_status_.load(std::memory_order_acquire);
        s.elapsed_seconds = secs;
        s.throughput_meps =
            (secs > 0.0) ? (static_cast<double>(s.consumed_events) / secs / 1e6) : 0.0;
        s.min_ns = histogram_.min_ns();
        s.p50_ns = histogram_.p50();
        s.p90_ns = histogram_.p90();
        s.p99_ns = histogram_.p99();
        s.p99_9_ns = histogram_.p99_9();
        s.max_ns = histogram_.max_ns();
        s.mean_ns = histogram_.mean_ns();
        return s;
    }

    [[nodiscard]] RingStatus last_status() const noexcept {
        return last_status_.load(std::memory_order_acquire);
    }
    [[nodiscard]] const LatencyHistogram& histogram() const noexcept { return histogram_; }
    [[nodiscard]] const std::string& sink_error() const noexcept { return sink_.last_error(); }

private:
    struct alignas(kCacheLineSize) StampedFrame {
        EventFrame frame{};
        std::uint64_t enqueue_steady_ns{0};
    };

    static std::uint64_t monotonic_now_ns() noexcept {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }

    std::size_t filter_via_soa_staging(std::size_t n) noexcept {
        std::size_t total_kept = 0;
        std::size_t offset = 0;
        while (offset < n) {
            const std::size_t chunk = std::min(n - offset, kColumnarPageEvents);
            ColumnarPage& page = *soa_staging_;
            page.header.event_count = static_cast<std::uint32_t>(chunk);
            for (std::size_t i = 0; i < chunk; ++i) {
                const EventFrame& f = batch_stamped_[offset + i].frame;
                page.energy_mev[i] = f.energy_mev;
                page.timing_ps[i] = f.timing_ps;
                page.pileup_score[i] = f.pileup_score;
                page.quality_flags[i] = f.quality_flags;
                page.multiplicity[i] = f.multiplicity;
                page.trigger_mask[i] = f.trigger_mask;
                if (config_.trigger.custom_predicate != nullptr) {
                    page.timestamp_ns[i] = f.timestamp_ns;
                    page.sequence_id[i] = f.sequence_id;
                    page.detector_id[i] = f.detector_id;
                    page.channel_id[i] = f.channel_id;
                    page.pos_x_mm[i] = f.pos_x_mm;
                    page.pos_y_mm[i] = f.pos_y_mm;
                    page.pos_z_mm[i] = f.pos_z_mm;
                }
            }
            const std::size_t words_needed = (chunk + 63u) / 64u;
            (void)evaluate_columnar_page(page, config_.trigger, bitmask_words_.data(),
                                         config_.simd_backend);
            for (std::size_t w = 0; w < words_needed; ++w) {
                std::uint64_t word = bitmask_words_[w];
                const std::size_t base = w * 64u;
                while (word != 0u) {
                    const unsigned bit = static_cast<unsigned>(std::countr_zero(word));
                    const std::size_t idx = base + bit;
                    if (idx < chunk) {
                        batch_filtered_[total_kept++] =
                            batch_stamped_[offset + idx].frame;
                    }
                    word &= (word - 1u);
                }
            }
            offset += chunk;
        }
        return total_kept;
    }

    void process_batch(std::size_t n) noexcept {
        std::size_t kept = 0;
        if (config_.use_soa_staging && n >= 8) {
            kept = filter_via_soa_staging(n);
        } else {
            for (std::size_t i = 0; i < n; ++i) {
                batch_frames_[i] = batch_stamped_[i].frame;
            }
            kept = filter_events(batch_frames_.data(), n, config_.trigger,
                                 batch_filtered_.data(), config_.simd_backend);
        }

        if (kept > 0 && sink_.is_open()) {
            const std::size_t wrote = sink_.write_batch(batch_filtered_.data(), kept);
            pages_written_.store(sink_.pages_written(), std::memory_order_relaxed);
            if (wrote < kept) {
                sink_errors_.fetch_add(1, std::memory_order_relaxed);
                last_status_.store(sink_.last_status(), std::memory_order_release);
                emit_log(LogLevel::Error, sink_.last_status(), "pipeline",
                         "ColumnarSink partial/failed batch write", kept - wrote);
            }
        }
        const std::uint64_t done_ns = monotonic_now_ns();
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint64_t lat =
                (done_ns >= batch_stamped_[i].enqueue_steady_ns)
                    ? (done_ns - batch_stamped_[i].enqueue_steady_ns)
                    : 0ULL;
            histogram_.record(lat);
        }
        consumed_.fetch_add(n, std::memory_order_relaxed);
        passed_.fetch_add(kept, std::memory_order_relaxed);
        rejected_.fetch_add(n - kept, std::memory_order_relaxed);
    }

    void consumer_loop() {
        if (config_.consumer_cpu_core >= 0) {
            (void)pin_current_thread_to_core(config_.consumer_cpu_core);
        }
        const std::size_t max_batch = batch_stamped_.size();
        for (;;) {
            const std::size_t n = ring_->try_pop_batch(batch_stamped_.data(), max_batch);
            if (n > 0) {
                process_batch(n);
                continue;
            }
            if (stop_requested_.load(std::memory_order_acquire)) {
                while (const std::size_t rem =
                           ring_->try_pop_batch(batch_stamped_.data(), max_batch)) {
                    process_batch(rem);
                }
                break;
            }
            std::this_thread::yield();
        }
    }

    PipelineConfig config_;
    std::unique_ptr<SpscRingBuffer<StampedFrame, kRingCapacity>> ring_;
    std::unique_ptr<ColumnarPage> soa_staging_;
    std::array<std::uint64_t, kColumnarPageEvents / 64u> bitmask_words_{};
    ColumnarSink sink_;
    LatencyHistogram histogram_;

    std::vector<StampedFrame> batch_stamped_;
    std::vector<EventFrame> batch_frames_;
    std::vector<EventFrame> batch_filtered_;

    std::thread consumer_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};

    std::atomic<std::uint64_t> submitted_{0};
    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<std::uint64_t> consumed_{0};
    std::atomic<std::uint64_t> passed_{0};
    std::atomic<std::uint64_t> rejected_{0};
    std::atomic<std::uint64_t> pages_written_{0};
    std::atomic<std::uint64_t> sink_errors_{0};
    std::atomic<std::uint64_t> backpressure_waits_{0};
    std::atomic<RingStatus> last_status_{RingStatus::Ok};

    std::chrono::steady_clock::time_point start_time_{};
    std::chrono::steady_clock::time_point end_time_{};
};

[[nodiscard]] inline std::string to_json(const PipelineConfig& cfg) {
    std::ostringstream oss;
    oss << "{\n"
        << "  \"output_path\": \"" << cfg.output_path << "\",\n"
        << "  \"consumer_cpu_core\": " << cfg.consumer_cpu_core << ",\n"
        << "  \"batch_size\": " << cfg.batch_size << ",\n"
        << "  \"initial_page_capacity\": " << cfg.initial_page_capacity << ",\n"
        << "  \"projection_mask\": " << cfg.projection_mask << ",\n"
        << "  \"use_soa_staging\": " << (cfg.use_soa_staging ? "true" : "false") << ",\n"
        << "  \"trigger\": " << to_json(cfg.trigger) << "\n"
        << "}";
    return oss.str();
}

[[nodiscard]] inline bool from_json(std::string_view json, PipelineConfig& out_cfg) noexcept {
    if (json.empty()) {
        return false;
    }
    PipelineConfig parsed = out_cfg;
    bool found = false;

    auto extract_token = [&](std::string_view key, std::string_view& tok) -> bool {
        std::string pattern = "\"";
        pattern.append(key);
        pattern.push_back('"');
        const std::size_t kpos = json.find(pattern);
        if (kpos == std::string_view::npos) {
            return false;
        }
        const std::size_t colon = json.find(':', kpos + pattern.size());
        if (colon == std::string_view::npos) {
            return false;
        }
        std::string_view rest = json.substr(colon + 1);
        while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.front()))) {
            rest.remove_prefix(1);
        }
        if (rest.empty()) {
            return false;
        }
        if (rest.front() == '"') {
            rest.remove_prefix(1);
            const std::size_t q = rest.find('"');
            if (q == std::string_view::npos) {
                return false;
            }
            tok = rest.substr(0, q);
            return true;
        }
        std::size_t end = 0;
        while (end < rest.size() && rest[end] != ',' && rest[end] != '}' &&
               !std::isspace(static_cast<unsigned char>(rest[end]))) {
            ++end;
        }
        tok = rest.substr(0, end);
        return !tok.empty();
    };

    std::string_view tok;
    if (extract_token("output_path", tok)) {
        parsed.output_path = std::string(tok);
        found = true;
    }
    if (extract_token("consumer_cpu_core", tok)) {
        int v = -1;
        if (std::from_chars(tok.data(), tok.data() + tok.size(), v).ec == std::errc{}) {
            parsed.consumer_cpu_core = v;
            found = true;
        }
    }
    if (extract_token("batch_size", tok)) {
        std::size_t v = 0;
        if (std::from_chars(tok.data(), tok.data() + tok.size(), v).ec == std::errc{}) {
            parsed.batch_size = v;
            found = true;
        }
    }
    if (extract_token("initial_page_capacity", tok)) {
        std::size_t v = 0;
        if (std::from_chars(tok.data(), tok.data() + tok.size(), v).ec == std::errc{}) {
            parsed.initial_page_capacity = v;
            found = true;
        }
    }
    if (extract_token("projection_mask", tok)) {
        std::uint16_t v = 0;
        if (std::from_chars(tok.data(), tok.data() + tok.size(), v).ec == std::errc{}) {
            parsed.projection_mask = v;
            found = true;
        }
    }
    if (extract_token("use_soa_staging", tok)) {
        if (tok == "true") {
            parsed.use_soa_staging = true;
            found = true;
        } else if (tok == "false") {
            parsed.use_soa_staging = false;
            found = true;
        }
    }
    if (from_json(json, parsed.trigger)) {
        found = true;
    }
    if (found) {
        out_cfg = std::move(parsed);
    }
    return found;
}

[[nodiscard]] inline std::string to_prometheus_text(const PipelineStats& stats) {
    std::ostringstream oss;
    oss << "# HELP chronoring_events_submitted_total Total events submitted to ingestion ring\n"
        << "# TYPE chronoring_events_submitted_total counter\n"
        << "chronoring_events_submitted_total " << stats.submitted_events << "\n"
        << "# HELP chronoring_events_dropped_total Events dropped due to full ring\n"
        << "# TYPE chronoring_events_dropped_total counter\n"
        << "chronoring_events_dropped_total " << stats.dropped_events << "\n"
        << "# HELP chronoring_events_consumed_total Total events processed by pipeline consumer\n"
        << "# TYPE chronoring_events_consumed_total counter\n"
        << "chronoring_events_consumed_total " << stats.consumed_events << "\n"
        << "# HELP chronoring_events_passed_total Events accepted by SIMD trigger engine\n"
        << "# TYPE chronoring_events_passed_total counter\n"
        << "chronoring_events_passed_total " << stats.passed_events << "\n"
        << "# HELP chronoring_events_rejected_total Events rejected by SIMD trigger engine\n"
        << "# TYPE chronoring_events_rejected_total counter\n"
        << "chronoring_events_rejected_total " << stats.rejected_events << "\n"
        << "# HELP chronoring_pages_written_total Columnar pages persisted to disk\n"
        << "# TYPE chronoring_pages_written_total counter\n"
        << "chronoring_pages_written_total " << stats.pages_written << "\n"
        << "# HELP chronoring_sink_errors_total Columnar sink write errors\n"
        << "# TYPE chronoring_sink_errors_total counter\n"
        << "chronoring_sink_errors_total " << stats.sink_errors << "\n"
        << "# HELP chronoring_backpressure_waits_total Producer waits on full ring\n"
        << "# TYPE chronoring_backpressure_waits_total counter\n"
        << "chronoring_backpressure_waits_total " << stats.backpressure_waits << "\n"
        << std::fixed << std::setprecision(3)
        << "# HELP chronoring_throughput_meps Pipeline throughput in million events/sec\n"
        << "# TYPE chronoring_throughput_meps gauge\n"
        << "chronoring_throughput_meps " << stats.throughput_meps << "\n"
        << "# HELP chronoring_latency_ns End-to-end processing latency in nanoseconds\n"
        << "# TYPE chronoring_latency_ns summary\n"
        << "chronoring_latency_ns{quantile=\"0.5\"} " << stats.p50_ns << "\n"
        << "chronoring_latency_ns{quantile=\"0.9\"} " << stats.p90_ns << "\n"
        << "chronoring_latency_ns{quantile=\"0.99\"} " << stats.p99_ns << "\n"
        << "chronoring_latency_ns{quantile=\"0.999\"} " << stats.p99_9_ns << "\n"
        << "chronoring_latency_min_ns " << stats.min_ns << "\n"
        << "chronoring_latency_max_ns " << stats.max_ns << "\n"
        << "chronoring_latency_mean_ns " << stats.mean_ns << "\n";
    return oss.str();
}

} // namespace chronoring
