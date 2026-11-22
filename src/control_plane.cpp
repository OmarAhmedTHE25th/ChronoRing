#include "chronoring/control_plane.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <string>

namespace chronoring {

namespace {

bool extract_json_token(std::string_view json,
                        std::string_view key,
                        std::string_view& tok) noexcept {
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
}

} // namespace

// ============================================================================
// LiveConfigStore
// ============================================================================

std::uint64_t LiveConfigStore::publish(LivePipelineConfig cfg) noexcept {
    const std::uint64_t next_gen = generation_.fetch_add(1, std::memory_order_acq_rel) + 1ULL;
    const std::size_t next_idx = static_cast<std::size_t>((next_gen - 1ULL) % kRingSlots);
    cfg.generation = next_gen;
    slots_[next_idx] = cfg;
    active_idx_.store(next_idx, std::memory_order_release);
    return next_gen;
}

Expected<std::uint64_t, RingStatus> LiveConfigStore::reload_from_json(
    std::string_view json) noexcept {
    if (json.empty()) {
        return unexpected(RingStatus::InvalidArgument);
    }
    LivePipelineConfig updated = snapshot();
    bool any_found = from_json(json, updated.trigger);

    std::string_view tok;
    if (extract_json_token(json, "projection_mask", tok)) {
        std::uint16_t v = 0;
        if (std::from_chars(tok.data(), tok.data() + tok.size(), v).ec == std::errc{}) {
            updated.projection_mask = v;
            any_found = true;
        }
    }
    if (extract_json_token(json, "enable_compression", tok)) {
        if (tok == "true" || tok == "1") {
            updated.enable_compression = true;
            any_found = true;
        } else if (tok == "false" || tok == "0") {
            updated.enable_compression = false;
            any_found = true;
        }
    }
    if (extract_json_token(json, "batch_size", tok)) {
        std::size_t v = 0;
        if (std::from_chars(tok.data(), tok.data() + tok.size(), v).ec == std::errc{} && v > 0) {
            updated.batch_size = v;
            any_found = true;
        }
    }
    if (extract_json_token(json, "consumer_cpu_core", tok)) {
        int v = -1;
        if (std::from_chars(tok.data(), tok.data() + tok.size(), v).ec == std::errc{}) {
            updated.consumer_cpu_core = v;
            any_found = true;
        }
    }
    if (extract_json_token(json, "routing_mode", tok)) {
        if (tok == "broadcast" || tok == "Broadcast") {
            updated.routing_mode = RoutingMode::Broadcast;
            any_found = true;
        } else if (tok == "round_robin" || tok == "RoundRobin") {
            updated.routing_mode = RoutingMode::RoundRobin;
            any_found = true;
        } else if (tok == "filtered" || tok == "PredicateFiltered") {
            updated.routing_mode = RoutingMode::PredicateFiltered;
            any_found = true;
        }
    }

    if (!any_found) {
        return unexpected(RingStatus::InvalidArgument);
    }
    return publish(updated);
}

// ============================================================================
// StreamingAnomalyDetector
// ============================================================================

void StreamingAnomalyDetector::RunningStats::update(double x, std::uint64_t n) noexcept {
    const double delta = x - mean;
    mean += delta / static_cast<double>(n);
    const double delta2 = x - mean;
    m2 += delta * delta2;
}

double StreamingAnomalyDetector::RunningStats::stddev(std::uint64_t n) const noexcept {
    if (n < 2) {
        return 0.0;
    }
    return std::sqrt(std::max(0.0, m2 / static_cast<double>(n - 1)));
}

double StreamingAnomalyDetector::latency_stddev() const noexcept {
    return lat_stats_.stddev(count_);
}

std::size_t StreamingAnomalyDetector::observe(double latency_ns,
                                              double reject_rate,
                                              double ring_fill_ratio,
                                              std::vector<AnomalyEvent>* out_events) {
    std::size_t detected_here = 0;

    if (count_ >= warmup_samples_) {
        const double lat_sd = std::max(lat_stats_.stddev(count_), lat_stats_.mean * 0.02 + 1e-6);
        const double lat_z = (latency_ns - lat_stats_.mean) / lat_sd;
        if (lat_z > z_threshold_) {
            ++detected_here;
            ++total_anomalies_;
            if (out_events != nullptr) {
                out_events->push_back(AnomalyEvent{
                    AnomalyKind::LatencySpike,
                    latency_ns,
                    lat_stats_.mean,
                    lat_z,
                    count_});
            }
        }

        const double rej_sd = std::max(rej_stats_.stddev(count_), 0.01);
        const double rej_z = (reject_rate - rej_stats_.mean) / rej_sd;
        if (rej_z > z_threshold_) {
            ++detected_here;
            ++total_anomalies_;
            if (out_events != nullptr) {
                out_events->push_back(AnomalyEvent{
                    AnomalyKind::RejectRateBurst,
                    reject_rate,
                    rej_stats_.mean,
                    rej_z,
                    count_});
            }
        }
    }

    if (ring_fill_ratio >= backpressure_limit_) {
        ++detected_here;
        ++total_anomalies_;
        if (out_events != nullptr) {
            out_events->push_back(AnomalyEvent{
                AnomalyKind::BackpressureSaturation,
                ring_fill_ratio,
                backpressure_limit_,
                (ring_fill_ratio - backpressure_limit_) / 0.05,
                count_});
        }
    }

    // Update running Welford baseline on nominal samples (don't poison baseline with extreme spikes).
    if (detected_here == 0 || count_ < warmup_samples_) {
        ++count_;
        lat_stats_.update(latency_ns, count_);
        rej_stats_.update(reject_rate, count_);
        lat_mean_ = lat_stats_.mean;
        rej_mean_ = rej_stats_.mean;
    }

    return detected_here;
}

// ============================================================================
// PipelineAutoTuner
// ============================================================================

AutoTunerDecision PipelineAutoTuner::recommend(double ring_fill_ratio,
                                               double p99_ns,
                                               std::size_t current_batch_size,
                                               int current_core,
                                               NumaPolicy current_numa) const noexcept {
    AutoTunerDecision dec;
    dec.batch_size = std::clamp(current_batch_size, min_batch_, max_batch_);
    dec.cpu_core = current_core;
    dec.numa_policy = current_numa;

    if (ring_fill_ratio >= 0.70) {
        dec.batch_size = std::min(max_batch_, dec.batch_size * 2U);
    } else if (ring_fill_ratio <= 0.15 && p99_ns > target_p99_ns_) {
        dec.batch_size = std::max(min_batch_, dec.batch_size / 2U);
    }

    if (p99_ns > target_p99_ns_ || ring_fill_ratio >= 0.75) {
        if (dec.cpu_core < 0) {
            const std::uint32_t cpus = available_cpu_count();
            dec.cpu_core = (cpus > 1U) ? 1 : 0;
        }
        if (dec.numa_policy == NumaPolicy::Default) {
            dec.numa_policy = NumaPolicy::BindLocalNode;
        }
    }

    dec.changed = (dec.batch_size != current_batch_size) ||
                  (dec.cpu_core != current_core) ||
                  (dec.numa_policy != current_numa);
    return dec;
}

} // namespace chronoring
