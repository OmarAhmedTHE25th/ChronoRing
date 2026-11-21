#include "chronoring/event_frame.hpp"
#include "chronoring/latency_histogram.hpp"
#include "chronoring/mpsc_ring.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace chronoring::test {

TEST(MpscRingTest, SingleThreadedWrapAroundAndBatchPop) {
    MpscRingBuffer<std::uint64_t, 8> ring;
    EXPECT_EQ(ring.capacity(), 8U);
    EXPECT_TRUE(ring.empty());

    for (std::uint64_t round = 0; round < 10; ++round) {
        for (std::uint64_t i = 0; i < 8; ++i) {
            EXPECT_TRUE(ring.try_push(round * 100ULL + i));
        }
        EXPECT_FALSE(ring.try_push(9999ULL));
        EXPECT_EQ(ring.size_approx(), 8U);

        std::uint64_t out[4]{};
        EXPECT_EQ(ring.try_pop_batch(out, 4), 4U);
        for (std::uint64_t i = 0; i < 4; ++i) {
            EXPECT_EQ(out[i], round * 100ULL + i);
        }
        for (std::uint64_t i = 4; i < 8; ++i) {
            std::uint64_t val = 0;
            EXPECT_TRUE(ring.try_pop(val));
            EXPECT_EQ(val, round * 100ULL + i);
        }
        std::uint64_t extra = 0;
        EXPECT_FALSE(ring.try_pop(extra));
        EXPECT_TRUE(ring.empty());
    }
}

TEST(MpscRingTest, MultiProducerContentionAndLinearization) {
    constexpr std::size_t kProducers = 4;
    constexpr std::size_t kPerProducer = 25'000;
    constexpr std::size_t kTotal = kProducers * kPerProducer;

    auto ring = std::make_unique<MpscRingBuffer<EventFrame, 1024>>();
    std::atomic<bool> start_flag{false};

    std::vector<std::thread> producers;
    producers.reserve(kProducers);
    for (std::size_t p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p]() {
            while (!start_flag.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (std::size_t i = 0; i < kPerProducer; ++i) {
                EventFrame ev{};
                ev.detector_id = static_cast<std::uint32_t>(p);
                ev.sequence_id = i;
                ev.timestamp_ns = (p << 32) | i;
                while (!ring->try_push(ev)) {
                    std::this_thread::yield();
                }
            }
        });
    }

    std::vector<std::size_t> next_expected(kProducers, 0);
    std::size_t popped = 0;

    std::thread consumer([&]() {
        start_flag.store(true, std::memory_order_release);
        EventFrame ev{};
        while (popped < kTotal) {
            if (ring->try_pop(ev)) {
                ASSERT_LT(ev.detector_id, kProducers);
                EXPECT_EQ(ev.sequence_id, next_expected[ev.detector_id]);
                EXPECT_EQ(ev.timestamp_ns,
                          (static_cast<std::uint64_t>(ev.detector_id) << 32) |
                              ev.sequence_id);
                ++next_expected[ev.detector_id];
                ++popped;
            } else {
                std::this_thread::yield();
            }
        }
    });

    for (auto& t : producers) {
        t.join();
    }
    consumer.join();

    EXPECT_EQ(popped, kTotal);
    for (std::size_t p = 0; p < kProducers; ++p) {
        EXPECT_EQ(next_expected[p], kPerProducer);
    }
    EXPECT_TRUE(ring->empty());
}

TEST(LatencyHistogramTest, ConcurrentRecordingAndQuantiles) {
    LatencyHistogram hist;
    EXPECT_EQ(hist.count(), 0U);
    EXPECT_EQ(hist.p50(), 0U);

    constexpr int kThreads = 4;
    constexpr int kSamples = 10'000;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&]() {
            for (int i = 1; i <= kSamples; ++i) {
                hist.record(static_cast<std::uint64_t>(i * 10));
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }

    EXPECT_EQ(hist.count(), static_cast<std::uint64_t>(kThreads * kSamples));
    EXPECT_EQ(hist.min_ns(), 10U);
    EXPECT_EQ(hist.max_ns(), 100'000U);
    EXPECT_LE(hist.p50(), hist.p90());
    EXPECT_LE(hist.p90(), hist.p99());
    EXPECT_LE(hist.p99(), hist.p99_9());
    EXPECT_LE(hist.p99_9(), hist.max_ns());
    EXPECT_NEAR(static_cast<double>(hist.p50()), 50'000.0, 1'000.0);
    EXPECT_NEAR(static_cast<double>(hist.p99()), 99'000.0, 2'000.0);
}

} // namespace chronoring::test
