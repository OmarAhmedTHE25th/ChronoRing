#pragma once

#include "chronoring/columnar_sink.hpp"
#include "chronoring/event_frame.hpp"
#include "chronoring/latency_histogram.hpp"
#include "chronoring/simd_trigger.hpp"
#include "chronoring/spsc_ring.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
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
};

struct PipelineStats {
    std::uint64_t submitted_events{0};
    std::uint64_t dropped_events{0};
    std::uint64_t consumed_events{0};
    std::uint64_t passed_events{0};
    std::uint64_t rejected_events{0};
    std::uint64_t pages_written{0};
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
          ring_(std::make_unique<SpscRingBuffer<StampedFrame, kRingCapacity>>()) {
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
        if (!config_.output_path.empty()) {
            if (!sink_.open(config_.output_path, config_.initial_page_capacity)) {
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
        return false;
    }

    void submit_wait(const EventFrame& frame) noexcept {
        const std::uint64_t now_ns = monotonic_now_ns();
        const StampedFrame sf{frame, now_ns};
        while (!ring_->try_push(sf)) {
            if (stop_requested_.load(std::memory_order_acquire)) {
                return;
            }
            std::this_thread::yield();
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

    void process_batch(std::size_t n) noexcept {
        for (std::size_t i = 0; i < n; ++i) {
            batch_frames_[i] = batch_stamped_[i].frame;
        }
        const std::size_t kept =
            filter_events(batch_frames_.data(), n, config_.trigger,
                          batch_filtered_.data(), config_.simd_backend);
        if (kept > 0 && sink_.is_open()) {
            (void)sink_.write_batch(batch_filtered_.data(), kept);
            pages_written_.store(sink_.pages_written(), std::memory_order_relaxed);
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
                // One final drain after observing stop_requested_
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

    std::chrono::steady_clock::time_point start_time_{};
    std::chrono::steady_clock::time_point end_time_{};
};

} // namespace chronoring
