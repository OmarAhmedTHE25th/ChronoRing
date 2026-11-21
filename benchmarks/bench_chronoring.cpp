#include "chronoring/columnar_sink.hpp"
#include "chronoring/event_frame.hpp"
#include "chronoring/mpsc_ring.hpp"
#include "chronoring/simd_trigger.hpp"
#include "chronoring/spsc_ring.hpp"

#include <benchmark/benchmark.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace {

using namespace chronoring;

EventFrame make_bench_frame(std::uint64_t seq) noexcept {
    EventFrame f{};
    f.timestamp_ns = 1'000'000ULL + seq * 10ULL;
    f.sequence_id = seq;
    f.detector_id = static_cast<std::uint32_t>(seq & 15U);
    f.channel_id = static_cast<std::uint32_t>(seq & 255U);
    f.energy_mev = (seq % 5U == 0U) ? 0.1f : 42.5f + static_cast<float>(seq % 100U);
    f.timing_ps = static_cast<float>(static_cast<int>(seq % 300U) - 150);
    f.pileup_score = 0.15f;
    f.quality_flags = kFlagValid | kFlagCalibrated;
    f.multiplicity = 2;
    f.trigger_mask = 0x1u;
    return f;
}

void BM_MutexQueue_EventFrame(benchmark::State& state) {
    constexpr std::size_t kBatch = 10'000;
    const EventFrame sample = make_bench_frame(1);

    for (auto _ : state) {
        std::mutex mtx;
        std::condition_variable cv_not_empty;
        std::queue<EventFrame> q;
        bool done = false;

        std::thread producer([&]() {
            for (std::size_t i = 0; i < kBatch; ++i) {
                {
                    std::lock_guard<std::mutex> lock(mtx);
                    q.push(sample);
                }
                cv_not_empty.notify_one();
            }
            {
                std::lock_guard<std::mutex> lock(mtx);
                done = true;
            }
            cv_not_empty.notify_one();
        });

        std::size_t popped = 0;
        while (popped < kBatch) {
            std::unique_lock<std::mutex> lock(mtx);
            cv_not_empty.wait(lock, [&]() { return !q.empty() || done; });
            while (!q.empty()) {
                EventFrame out = q.front();
                q.pop();
                benchmark::DoNotOptimize(out);
                ++popped;
            }
        }
        producer.join();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(kBatch));
    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(kBatch * sizeof(EventFrame)));
}
BENCHMARK(BM_MutexQueue_EventFrame);

void BM_SpscRing_EventFrame(benchmark::State& state) {
    constexpr std::size_t kBatch = 10'000;
    const EventFrame sample = make_bench_frame(1);
    auto ring = std::make_unique<SpscRingBuffer<EventFrame, 4096>>();

    for (auto _ : state) {
        std::thread producer([&]() {
            for (std::size_t i = 0; i < kBatch; ++i) {
                while (!ring->try_push(sample)) {
                    std::this_thread::yield();
                }
            }
        });

        std::size_t popped = 0;
        EventFrame out{};
        while (popped < kBatch) {
            if (ring->try_pop(out)) {
                benchmark::DoNotOptimize(out);
                ++popped;
            }
        }
        producer.join();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(kBatch));
    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(kBatch * sizeof(EventFrame)));
}
BENCHMARK(BM_SpscRing_EventFrame);

void BM_SpscRing_SingleThreadPushPop(benchmark::State& state) {
    auto ring = std::make_unique<SpscRingBuffer<EventFrame, 1024>>();
    const EventFrame sample = make_bench_frame(7);
    EventFrame out{};
    for (auto _ : state) {
        bool ok = ring->try_push(sample);
        benchmark::DoNotOptimize(ok);
        ok = ring->try_pop(out);
        benchmark::DoNotOptimize(out);
    }
    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(sizeof(EventFrame)));
}
BENCHMARK(BM_SpscRing_SingleThreadPushPop);

void BM_MpscRing_MultiProducer(benchmark::State& state) {
    const std::size_t num_producers = static_cast<std::size_t>(state.range(0));
    constexpr std::size_t kPerProducer = 5'000;
    const std::size_t total = num_producers * kPerProducer;
    const EventFrame sample = make_bench_frame(3);
    auto ring = std::make_unique<MpscRingBuffer<EventFrame, 4096>>();

    for (auto _ : state) {
        std::vector<std::thread> producers;
        producers.reserve(num_producers);
        for (std::size_t p = 0; p < num_producers; ++p) {
            producers.emplace_back([&]() {
                for (std::size_t i = 0; i < kPerProducer; ++i) {
                    while (!ring->try_push(sample)) {
                        std::this_thread::yield();
                    }
                }
            });
        }
        std::size_t popped = 0;
        EventFrame out{};
        while (popped < total) {
            if (ring->try_pop(out)) {
                benchmark::DoNotOptimize(out);
                ++popped;
            }
        }
        for (auto& th : producers) {
            th.join();
        }
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(total));
}
BENCHMARK(BM_MpscRing_MultiProducer)->Arg(1)->Arg(2)->Arg(4);

void BM_SimdTrigger_AoS(benchmark::State& state, SimdBackend backend) {
    constexpr std::size_t kCount = 4096;
    std::vector<EventFrame> in(kCount);
    std::vector<EventFrame> out(kCount);
    for (std::size_t i = 0; i < kCount; ++i) {
        in[i] = make_bench_frame(i);
    }
    const TriggerConfig cfg{};
    for (auto _ : state) {
        std::size_t kept = filter_events(in.data(), kCount, cfg, out.data(), backend);
        benchmark::DoNotOptimize(kept);
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(kCount));
    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(kCount * sizeof(EventFrame)));
}

BENCHMARK_CAPTURE(BM_SimdTrigger_AoS, Scalar, SimdBackend::Scalar);
BENCHMARK_CAPTURE(BM_SimdTrigger_AoS, SSE42, SimdBackend::SSE42);
BENCHMARK_CAPTURE(BM_SimdTrigger_AoS, AVX2, SimdBackend::AVX2);

void BM_SimdTrigger_SoA(benchmark::State& state, SimdBackend backend) {
    std::vector<EventFrame> in(kColumnarPageEvents);
    for (std::size_t i = 0; i < kColumnarPageEvents; ++i) {
        in[i] = make_bench_frame(i);
    }
    auto page = std::make_unique<ColumnarPage>();
    transpose_aos_to_soa(in.data(), kColumnarPageEvents, *page);
    std::uint64_t words[kColumnarPageEvents / 64]{};
    const TriggerConfig cfg{};

    for (auto _ : state) {
        std::size_t kept = evaluate_columnar_page(*page, cfg, words, backend);
        benchmark::DoNotOptimize(kept);
        benchmark::DoNotOptimize(words);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(kColumnarPageEvents));
}

BENCHMARK_CAPTURE(BM_SimdTrigger_SoA, Scalar, SimdBackend::Scalar);
BENCHMARK_CAPTURE(BM_SimdTrigger_SoA, AVX2, SimdBackend::AVX2);

void BM_Crc32c_SW_Page(benchmark::State& state) {
    auto page = std::make_unique<ColumnarPage>();
    for (auto _ : state) {
        std::uint32_t c = crc32c_sw(page.get(), sizeof(ColumnarPage));
        benchmark::DoNotOptimize(c);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(sizeof(ColumnarPage)));
}
BENCHMARK(BM_Crc32c_SW_Page);

void BM_Crc32c_HW_Page(benchmark::State& state) {
    auto page = std::make_unique<ColumnarPage>();
    for (auto _ : state) {
        std::uint32_t c = crc32c_hw(page.get(), sizeof(ColumnarPage));
        benchmark::DoNotOptimize(c);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(sizeof(ColumnarPage)));
}
BENCHMARK(BM_Crc32c_HW_Page);

void BM_ColumnarTranspose_AoS_to_SoA(benchmark::State& state) {
    std::vector<EventFrame> in(kColumnarPageEvents);
    for (std::size_t i = 0; i < kColumnarPageEvents; ++i) {
        in[i] = make_bench_frame(i);
    }
    auto page = std::make_unique<ColumnarPage>();
    for (auto _ : state) {
        transpose_aos_to_soa(in.data(), kColumnarPageEvents, *page);
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(kColumnarPageEvents));
    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(sizeof(ColumnarPage)));
}
BENCHMARK(BM_ColumnarTranspose_AoS_to_SoA);

} // namespace

int main(int argc, char** argv) {
    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv)) {
        return 1;
    }
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
