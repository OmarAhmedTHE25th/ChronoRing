#include "chronoring/chronoring.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace chronoring;

EventFrame make_synthetic_event(std::uint64_t seq) noexcept {
    EventFrame ev{};
    ev.timestamp_ns = 1'700'000'000'000ULL + seq * 100ULL;
    ev.sequence_id = seq;
    ev.detector_id = static_cast<std::uint32_t>(seq % 16U);
    ev.channel_id = static_cast<std::uint32_t>(seq % 256U);
    ev.energy_mev = (seq % 10U == 0U) ? 0.1f : (10.0f + static_cast<float>(seq % 500U));
    ev.timing_ps = static_cast<float>((static_cast<int>(seq % 401U)) - 200);
    ev.pos_x_mm = static_cast<float>(seq % 120U) - 60.0f;
    ev.pos_y_mm = static_cast<float>((seq * 3U) % 120U) - 60.0f;
    ev.pos_z_mm = static_cast<float>((seq * 7U) % 200U) - 100.0f;
    ev.pileup_score = (seq % 17U == 0U) ? 0.85f : 0.12f;
    ev.quality_flags = kFlagValid | kFlagCalibrated;
    if (seq % 29U == 0U) {
        ev.quality_flags |= kFlagVeto;
    }
    ev.multiplicity = static_cast<std::uint16_t>((seq % 8U) + 1U);
    ev.trigger_mask = static_cast<std::uint16_t>(1U << (seq % 4U));
    return ev;
}

void print_usage(const char* argv0) {
    std::cout << "ChronoRing v" << version()
              << " — Lock-Free Ring-Buffer Telemetry & SIMD Trigger Engine\n\n"
              << "Usage:\n"
              << "  " << argv0 << " --demo [event_count] [output_file]\n"
              << "  " << argv0 << " --json-report [report_path]\n"
              << "  " << argv0 << " --prometheus [event_count]\n"
              << "  " << argv0 << " --replay <crng_file>\n"
              << "  " << argv0 << " --shm-producer <shm_name> <event_count>\n"
              << "  " << argv0 << " --shm-consumer <shm_name> <event_count>\n"
              << "  " << argv0 << " --version\n"
              << "  " << argv0 << " --help\n";
}

PipelineStats run_pipeline_benchmark(std::size_t event_count, const std::string& out_path,
                                     bool& crc_ok, std::size_t& verified_pages,
                                     std::size_t& verified_events) {
    PipelineConfig cfg;
    cfg.output_path = out_path;
    cfg.simd_backend = SimdBackend::Auto;
    cfg.batch_size = 256;

    TelemetryPipeline pipeline(cfg);
    if (!pipeline.start()) {
        std::cerr << "Failed to start pipeline: " << pipeline.sink_error() << "\n";
        crc_ok = false;
        return {};
    }

    for (std::size_t i = 0; i < event_count; ++i) {
        pipeline.submit_wait(make_synthetic_event(i));
    }
    pipeline.stop();

    ColumnarReader reader(out_path);
    verified_pages = reader.num_pages();
    verified_events = reader.total_events();
    std::size_t bad_page = 0;
    crc_ok = reader.is_open() && reader.verify_all_pages(&bad_page);
    reader.close();

    return pipeline.stats();
}

int run_demo(std::size_t event_count, const std::string& explicit_out) {
    const bool use_temp = explicit_out.empty();
    const std::filesystem::path out_path =
        use_temp ? (std::filesystem::temp_directory_path() / "chronoring_demo.crng")
                 : std::filesystem::path(explicit_out);

    bool crc_ok = false;
    std::size_t verified_pages = 0;
    std::size_t verified_events = 0;
    const PipelineStats stats = run_pipeline_benchmark(
        event_count, out_path.string(), crc_ok, verified_pages, verified_events);

    if (use_temp) {
        std::error_code ec;
        std::filesystem::remove(out_path, ec);
    }

    std::cout << "=== ChronoRing v" << version() << " Live Telemetry Pipeline Demo ===\n"
              << "SIMD Backend      : " << backend_name(active_backend()) << "\n"
              << "Events Submitted  : " << stats.submitted_events << "\n"
              << "Events Consumed   : " << stats.consumed_events << "\n"
              << "Triggered Passed  : " << stats.passed_events << "\n"
              << "Trigger Rejected  : " << stats.rejected_events << "\n"
              << "Columnar Pages    : " << verified_pages << "\n"
              << "Persisted Events  : " << verified_events << "\n"
              << "CRC32C Integrity  : " << (crc_ok ? "VERIFIED" : "FAILED") << "\n"
              << "Pipeline Status   : " << status_string(stats.last_status) << "\n"
              << std::fixed << std::setprecision(3)
              << "Elapsed (s)       : " << stats.elapsed_seconds << "\n"
              << "Throughput (Meps) : " << stats.throughput_meps << "\n"
              << "Latency min (ns)  : " << stats.min_ns << "\n"
              << "Latency p50 (ns)  : " << stats.p50_ns << "\n"
              << "Latency p90 (ns)  : " << stats.p90_ns << "\n"
              << "Latency p99 (ns)  : " << stats.p99_ns << "\n"
              << "Latency p99.9 (ns): " << stats.p99_9_ns << "\n"
              << "Latency max (ns)  : " << stats.max_ns << "\n";

    return (crc_ok && stats.last_status == RingStatus::Ok) ? 0 : 1;
}

int run_prometheus(std::size_t event_count) {
    const std::filesystem::path out_path =
        std::filesystem::temp_directory_path() / "chronoring_prom.crng";
    bool crc_ok = false;
    std::size_t verified_pages = 0;
    std::size_t verified_events = 0;
    const PipelineStats stats = run_pipeline_benchmark(
        event_count, out_path.string(), crc_ok, verified_pages, verified_events);
    {
        std::error_code ec;
        std::filesystem::remove(out_path, ec);
    }
    std::cout << to_prometheus_text(stats);
    return crc_ok ? 0 : 1;
}

int run_replay(const std::string& file_path) {
    ColumnarReader reader(file_path);
    if (!reader.is_open() && reader.num_pages() == 0) {
        std::cerr << "Failed to open columnar file '" << file_path
                  << "': " << reader.last_error() << "\n";
        return 1;
    }

    const TriggerConfig cfg{};
    std::uint64_t total_scanned = 0;
    const auto t0 = std::chrono::steady_clock::now();
    const Expected<std::size_t, RingStatus> res =
        reader.replay_filtered(cfg, SimdBackend::Auto, &total_scanned);
    const auto t1 = std::chrono::steady_clock::now();

    if (!res) {
        std::cerr << "Replay failed with status: " << status_string(res.error()) << "\n";
        return 1;
    }

    const double secs = std::chrono::duration<double>(t1 - t0).count();
    const double meps =
        (secs > 0.0) ? (static_cast<double>(total_scanned) / secs / 1e6) : 0.0;

    std::cout << "=== ChronoRing Offline SoA Replay ===\n"
              << "File              : " << file_path << "\n"
              << "SIMD Backend      : " << backend_name(active_backend()) << "\n"
              << "Pages Verified    : " << reader.num_pages() << "\n"
              << "Events Scanned    : " << total_scanned << "\n"
              << "Triggered Passed  : " << res.value() << "\n"
              << std::fixed << std::setprecision(3)
              << "Elapsed (s)       : " << secs << "\n"
              << "Replay Rate (Meps): " << meps << "\n";
    return 0;
}

int run_json_report(const std::string& report_path) {
    constexpr std::size_t kSimdBatch = 16384;
    std::vector<EventFrame> input(kSimdBatch);
    std::vector<EventFrame> output(kSimdBatch);
    for (std::size_t i = 0; i < kSimdBatch; ++i) {
        input[i] = make_synthetic_event(i);
    }
    const TriggerConfig tcfg{};

    constexpr int kIterations = 200;
    auto bench_filter = [&](SimdBackend backend, std::size_t& out_passed) -> double {
        const auto t0 = std::chrono::steady_clock::now();
        std::size_t acc = 0;
        for (int it = 0; it < kIterations; ++it) {
            acc += filter_events(input.data(), kSimdBatch, tcfg, output.data(), backend);
        }
        out_passed = acc;
        const auto t1 = std::chrono::steady_clock::now();
        const double secs = std::chrono::duration<double>(t1 - t0).count();
        return (secs > 0.0)
                   ? (static_cast<double>(kSimdBatch) * kIterations / secs / 1e6)
                   : 0.0;
    };

    std::size_t scalar_passed = 0;
    std::size_t active_passed = 0;
    const double scalar_meps = bench_filter(SimdBackend::Scalar, scalar_passed);
    const double active_meps = bench_filter(SimdBackend::Auto, active_passed);

    const std::filesystem::path tmp_file =
        std::filesystem::temp_directory_path() / "chronoring_report.crng";
    bool crc_ok = false;
    std::size_t verified_pages = 0;
    std::size_t verified_events = 0;
    const PipelineStats stats = run_pipeline_benchmark(
        100'000, tmp_file.string(), crc_ok, verified_pages, verified_events);
    {
        std::error_code ec;
        std::filesystem::remove(tmp_file, ec);
    }

    std::ostringstream js;
    js << std::fixed << std::setprecision(3)
       << "{\n"
       << "  \"project\": \"ChronoRing\",\n"
       << "  \"version\": \"" << version() << "\",\n"
       << "  \"simd_backend\": \"" << backend_name(active_backend()) << "\",\n"
       << "  \"avx2_supported\": " << (cpu_supports_avx2() ? "true" : "false") << ",\n"
       << "  \"sse42_supported\": " << (cpu_supports_sse42() ? "true" : "false") << ",\n"
       << "  \"simd_benchmark\": {\n"
       << "    \"scalar_meps\": " << scalar_meps << ",\n"
       << "    \"active_meps\": " << active_meps << ",\n"
       << "    \"speedup\": " << (scalar_meps > 0.0 ? active_meps / scalar_meps : 1.0) << ",\n"
       << "    \"bit_exact_match\": " << (scalar_passed == active_passed ? "true" : "false") << "\n"
       << "  },\n"
       << "  \"pipeline\": {\n"
       << "    \"submitted_events\": " << stats.submitted_events << ",\n"
       << "    \"passed_events\": " << stats.passed_events << ",\n"
       << "    \"pages_written\": " << verified_pages << ",\n"
       << "    \"crc32c_verified\": " << (crc_ok ? "true" : "false") << ",\n"
       << "    \"elapsed_seconds\": " << stats.elapsed_seconds << ",\n"
       << "    \"throughput_meps\": " << stats.throughput_meps << ",\n"
       << "    \"latency_ns\": {\n"
       << "      \"min\": " << stats.min_ns << ",\n"
       << "      \"p50\": " << stats.p50_ns << ",\n"
       << "      \"p90\": " << stats.p90_ns << ",\n"
       << "      \"p99\": " << stats.p99_ns << ",\n"
       << "      \"p99_9\": " << stats.p99_9_ns << ",\n"
       << "      \"max\": " << stats.max_ns << ",\n"
       << "      \"mean\": " << stats.mean_ns << "\n"
       << "    }\n"
       << "  }\n"
       << "}\n";

    const std::string json_str = js.str();
    std::cout << json_str;
    if (!report_path.empty()) {
        std::ofstream ofs(report_path);
        if (!ofs) {
            std::cerr << "Failed to write JSON report to " << report_path << "\n";
            return 1;
        }
        ofs << json_str;
    }
    return crc_ok ? 0 : 1;
}

int run_shm_producer(const std::string& name, std::size_t count) {
    ShmRingBuffer ring(name, ShmMode::Create, 4096, false);
    if (!ring.is_open()) {
        std::cerr << "Shm producer failed to create '" << name
                  << "': " << ring.last_error() << "\n";
        return 1;
    }
    for (std::size_t i = 0; i < count; ++i) {
        const EventFrame ev = make_synthetic_event(i);
        while (!ring.try_push(ev)) {
            std::this_thread::yield();
        }
    }
    ring.close();
    std::cout << "Shm producer sent " << count << " events on '" << name << "'\n";
    return 0;
}

int run_shm_consumer(const std::string& name, std::size_t expected_count) {
    ShmRingBuffer ring;
    for (int attempt = 0; attempt < 200; ++attempt) {
        ring = ShmRingBuffer(name, ShmMode::Open);
        if (ring.is_open()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!ring.is_open()) {
        std::cerr << "Shm consumer failed to open '" << name
                  << "': " << ring.last_error() << "\n";
        return 1;
    }

    std::size_t received = 0;
    bool seq_ok = true;
    EventFrame ev{};
    while (received < expected_count) {
        if (ring.try_pop(ev)) {
            if (ev.sequence_id != received) {
                seq_ok = false;
            }
            ++received;
        } else if (ring.is_closed() && ring.empty()) {
            break;
        } else {
            std::this_thread::yield();
        }
    }

    ring.unmap();
    (void)ShmRingBuffer::unlink(name);
    std::cout << "Shm consumer received " << received << " events (sequence_ok="
              << (seq_ok ? "true" : "false") << ")\n";
    return (received == expected_count && seq_ok) ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc <= 1) {
        return run_demo(100'000, "");
    }

    const std::string cmd = argv[1];
    if (cmd == "--help" || cmd == "-h") {
        print_usage(argv[0]);
        return 0;
    }
    if (cmd == "--version" || cmd == "-v") {
        std::cout << "ChronoRing " << version() << "\n";
        return 0;
    }
    if (cmd == "--demo") {
        const std::size_t count = (argc >= 3) ? std::stoull(argv[2]) : 100'000ULL;
        const std::string out_file = (argc >= 4) ? argv[3] : "";
        return run_demo(count, out_file);
    }
    if (cmd == "--prometheus") {
        const std::size_t count = (argc >= 3) ? std::stoull(argv[2]) : 50'000ULL;
        return run_prometheus(count);
    }
    if (cmd == "--replay" && argc >= 3) {
        return run_replay(argv[2]);
    }
    if (cmd == "--json-report") {
        const std::string out_file = (argc >= 3) ? argv[2] : "";
        return run_json_report(out_file);
    }
    if (cmd == "--shm-producer" && argc >= 4) {
        return run_shm_producer(argv[2], std::stoull(argv[3]));
    }
    if (cmd == "--shm-consumer" && argc >= 4) {
        return run_shm_consumer(argv[2], std::stoull(argv[3]));
    }

    print_usage(argv[0]);
    return 1;
}
