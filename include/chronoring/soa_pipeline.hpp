#pragma once

#include "chronoring/columnar_sink.hpp"
#include "chronoring/event_frame.hpp"
#include "chronoring/hw_offload.hpp"
#include "chronoring/numa.hpp"
#include "chronoring/simd_trigger.hpp"
#include "chronoring/spsc_ring.hpp"
#include "chronoring/status.hpp"
#include "chronoring/trigger_jit.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace chronoring {

// Pure SoA column-to-column compaction guided by a 64-bit pass bitmask.
// Zero EventFrame AoS intermediates are constructed on the hot path.
[[nodiscard]] inline std::size_t compact_soa_page(const ColumnarPage& src,
                                                  const std::uint64_t* bitmask_words,
                                                  ColumnarPage& dst) noexcept {
    const std::size_t n =
        std::min<std::size_t>(src.header.event_count, kColumnarPageEvents);
    if (n == 0 || bitmask_words == nullptr) {
        dst.header.event_count = 0;
        return 0;
    }

    const std::size_t words = (n + 63u) / 64u;
    std::size_t out_idx = 0;

    for (std::size_t w = 0; w < words; ++w) {
        std::uint64_t mask = bitmask_words[w];
        const std::size_t base = w * 64u;
        while (mask != 0u) {
            const unsigned bit = static_cast<unsigned>(std::countr_zero(mask));
            const std::size_t i = base + bit;
            if (i < n) {
                dst.timestamp_ns[out_idx] = src.timestamp_ns[i];
                dst.sequence_id[out_idx] = src.sequence_id[i];
                dst.detector_id[out_idx] = src.detector_id[i];
                dst.channel_id[out_idx] = src.channel_id[i];
                dst.energy_mev[out_idx] = src.energy_mev[i];
                dst.timing_ps[out_idx] = src.timing_ps[i];
                dst.pos_x_mm[out_idx] = src.pos_x_mm[i];
                dst.pos_y_mm[out_idx] = src.pos_y_mm[i];
                dst.pos_z_mm[out_idx] = src.pos_z_mm[i];
                dst.pileup_score[out_idx] = src.pileup_score[i];
                dst.quality_flags[out_idx] = src.quality_flags[i];
                dst.multiplicity[out_idx] = src.multiplicity[i];
                dst.trigger_mask[out_idx] = src.trigger_mask[i];
                ++out_idx;
            }
            mask &= (mask - 1u);
        }
    }

    dst.header.event_count = static_cast<std::uint32_t>(out_idx);
    return out_idx;
}

// Edge adapter that populates a ColumnarPage directly in SoA layout without
// intermediate AoS staging buffers.
class SoaBatchBuilder {
public:
    explicit SoaBatchBuilder(ColumnarPage* target = nullptr) noexcept
        : target_(target) {}

    void bind(ColumnarPage* page) noexcept {
        target_ = page;
        if (target_ != nullptr) {
            target_->header.magic = kColumnarPageMagic;
            target_->header.version = kColumnarFormatVersion;
            target_->header.capacity = static_cast<std::uint32_t>(kColumnarPageEvents);
            target_->header.event_count = 0;
        }
    }

    [[nodiscard]] bool append_fields(std::uint64_t timestamp_ns,
                                     std::uint64_t sequence_id,
                                     std::uint32_t detector_id,
                                     std::uint32_t channel_id,
                                     float energy_mev,
                                     float timing_ps,
                                     float pileup_score,
                                     std::uint32_t quality_flags,
                                     std::uint16_t multiplicity,
                                     std::uint16_t trigger_mask,
                                     float pos_x_mm = 0.0f,
                                     float pos_y_mm = 0.0f,
                                     float pos_z_mm = 0.0f) noexcept {
        if (target_ == nullptr || target_->header.event_count >= kColumnarPageEvents) {
            return false;
        }
        const std::size_t idx = target_->header.event_count;
        target_->timestamp_ns[idx] = timestamp_ns;
        target_->sequence_id[idx] = sequence_id;
        target_->detector_id[idx] = detector_id;
        target_->channel_id[idx] = channel_id;
        target_->energy_mev[idx] = energy_mev;
        target_->timing_ps[idx] = timing_ps;
        target_->pos_x_mm[idx] = pos_x_mm;
        target_->pos_y_mm[idx] = pos_y_mm;
        target_->pos_z_mm[idx] = pos_z_mm;
        target_->pileup_score[idx] = pileup_score;
        target_->quality_flags[idx] = quality_flags;
        target_->multiplicity[idx] = multiplicity;
        target_->trigger_mask[idx] = trigger_mask;
        ++target_->header.event_count;
        return true;
    }

    [[nodiscard]] bool append_edge_frame(const EventFrame& f) noexcept {
        return append_fields(f.timestamp_ns, f.sequence_id, f.detector_id, f.channel_id,
                             f.energy_mev, f.timing_ps, f.pileup_score, f.quality_flags,
                             f.multiplicity, f.trigger_mask, f.pos_x_mm, f.pos_y_mm,
                             f.pos_z_mm);
    }

    [[nodiscard]] std::size_t count() const noexcept {
        return (target_ != nullptr) ? target_->header.event_count : 0;
    }
    [[nodiscard]] bool full() const noexcept {
        return count() >= kColumnarPageEvents;
    }

private:
    ColumnarPage* target_{nullptr};
};

struct SoaPipelineConfig {
    std::string output_path{};
    TriggerConfig trigger{};
    SimdBackend simd_backend{SimdBackend::Auto};
    const TriggerProgram* jit_program{nullptr};
    const OffloadStageRouter* offload_router{nullptr};
    int consumer_cpu_core{-1};
    std::size_t initial_page_capacity{64};
    std::uint16_t projection_mask{kColAll};
    bool enable_compression{false};
};

// Pure SoA zero-copy dataflow pipeline:
//   - Pre-allocated pool of `ColumnarPage`s (no heap allocations after start()).
//   - Producer acquires a free pool index, fills columns directly in SoA layout, and pushes index.
//   - Consumer evaluates SIMD / JIT / HW-offload trigger directly on input page, compacts columns
//     via `compact_soa_page`, and persists via `ColumnarSink::write_soa_page`.
class SoaDataflowPipeline {
public:
    static constexpr std::size_t kPoolSize = 64;

    explicit SoaDataflowPipeline(SoaPipelineConfig config)
        : config_(std::move(config)),
          pool_(kPoolSize),
          output_page_(std::make_unique<ColumnarPage>()) {}

    ~SoaDataflowPipeline() {
        stop();
    }

    SoaDataflowPipeline(const SoaDataflowPipeline&) = delete;
    SoaDataflowPipeline& operator=(const SoaDataflowPipeline&) = delete;

    [[nodiscard]] bool start() {
        if (running_.load(std::memory_order_acquire)) {
            return false;
        }
        last_status_.store(RingStatus::Ok, std::memory_order_relaxed);
        if (!config_.output_path.empty()) {
            if (!sink_.open(config_.output_path, config_.initial_page_capacity,
                            config_.projection_mask, config_.enable_compression)) {
                last_status_.store(sink_.last_status(), std::memory_order_release);
                sink_errors_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
        }
        std::size_t dummy = 0;
        while (free_indices_.try_pop(dummy)) {}
        while (filled_indices_.try_pop(dummy)) {}
        for (std::size_t i = 0; i < kPoolSize; ++i) {
            (void)free_indices_.try_push(i);
        }

        consumed_events_.store(0, std::memory_order_relaxed);
        passed_events_.store(0, std::memory_order_relaxed);
        pages_written_.store(0, std::memory_order_relaxed);
        sink_errors_.store(0, std::memory_order_relaxed);
        offload_executions_.store(0, std::memory_order_relaxed);
        stop_requested_.store(false, std::memory_order_release);
        running_.store(true, std::memory_order_release);
        consumer_thread_ = std::thread([this]() { consumer_loop(); });
        return true;
    }

    [[nodiscard]] ColumnarPage* acquire_input_page(std::size_t& out_pool_idx) noexcept {
        if (!free_indices_.try_pop(out_pool_idx)) {
            return nullptr;
        }
        ColumnarPage& p = pool_[out_pool_idx];
        p.header.magic = kColumnarPageMagic;
        p.header.version = kColumnarFormatVersion;
        p.header.capacity = static_cast<std::uint32_t>(kColumnarPageEvents);
        p.header.event_count = 0;
        return &p;
    }

    [[nodiscard]] bool submit_input_page(std::size_t pool_idx) noexcept {
        if (pool_idx >= kPoolSize || !running_.load(std::memory_order_acquire)) {
            return false;
        }
        return filled_indices_.try_push(pool_idx);
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
        if (sink_.is_open()) {
            sink_.close();
            pages_written_.store(sink_.pages_written(), std::memory_order_relaxed);
            if (sink_.last_status() != RingStatus::Ok) {
                last_status_.store(sink_.last_status(), std::memory_order_release);
                sink_errors_.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    [[nodiscard]] std::uint64_t consumed_events() const noexcept {
        return consumed_events_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t passed_events() const noexcept {
        return passed_events_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t pages_written() const noexcept {
        return pages_written_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t sink_errors() const noexcept {
        return sink_errors_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t offload_executions() const noexcept {
        return offload_executions_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] RingStatus last_status() const noexcept {
        return last_status_.load(std::memory_order_acquire);
    }

private:
    void process_page(std::size_t idx) noexcept {
        const ColumnarPage& in_page = pool_[idx];
        const std::size_t total =
            std::min<std::size_t>(in_page.header.event_count, kColumnarPageEvents);

        std::array<std::uint64_t, kColumnarPageEvents / 64u> words{};
        std::size_t kept = 0;

        if (config_.jit_program != nullptr) {
            kept = evaluate_trigger_program(*config_.jit_program, in_page, words.data());
        } else if (config_.offload_router != nullptr) {
            const OffloadResult res =
                config_.offload_router->execute_page(in_page, config_.trigger, words.data());
            if (res.status != RingStatus::Ok) {
                last_status_.store(res.status, std::memory_order_release);
            }
            kept = res.passed_events;
            offload_executions_.fetch_add(1, std::memory_order_relaxed);
        } else {
            kept = evaluate_columnar_page(in_page, config_.trigger, words.data(),
                                          config_.simd_backend);
        }

        if (kept > 0 && sink_.is_open()) {
            (void)compact_soa_page(in_page, words.data(), *output_page_);
            if (!sink_.write_soa_page(*output_page_)) {
                sink_errors_.fetch_add(1, std::memory_order_relaxed);
                last_status_.store(sink_.last_status(), std::memory_order_release);
            }
            pages_written_.store(sink_.pages_written(), std::memory_order_relaxed);
        }

        consumed_events_.fetch_add(total, std::memory_order_relaxed);
        passed_events_.fetch_add(kept, std::memory_order_relaxed);
        while (!free_indices_.try_push(idx)) {
            if (stop_requested_.load(std::memory_order_acquire)) {
                break;
            }
            std::this_thread::yield();
        }
    }

    void consumer_loop() {
        if (config_.consumer_cpu_core >= 0) {
            (void)pin_current_thread_to_core(config_.consumer_cpu_core);
        }
        std::size_t idx = 0;
        for (;;) {
            if (filled_indices_.try_pop(idx)) {
                process_page(idx);
                continue;
            }
            if (stop_requested_.load(std::memory_order_acquire)) {
                while (filled_indices_.try_pop(idx)) {
                    process_page(idx);
                }
                break;
            }
            std::this_thread::yield();
        }
    }

    SoaPipelineConfig config_;
    std::vector<ColumnarPage> pool_;
    std::unique_ptr<ColumnarPage> output_page_;
    SpscRingBuffer<std::size_t, kPoolSize> free_indices_{};
    SpscRingBuffer<std::size_t, kPoolSize> filled_indices_{};
    ColumnarSink sink_;

    std::thread consumer_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<std::uint64_t> consumed_events_{0};
    std::atomic<std::uint64_t> passed_events_{0};
    std::atomic<std::uint64_t> pages_written_{0};
    std::atomic<std::uint64_t> sink_errors_{0};
    std::atomic<std::uint64_t> offload_executions_{0};
    std::atomic<RingStatus> last_status_{RingStatus::Ok};
};

} // namespace chronoring
