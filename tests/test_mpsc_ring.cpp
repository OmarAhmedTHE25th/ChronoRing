#include "chronoring/event_frame.hpp"
#include "chronoring/latency_histogram.hpp"
#include "chronoring/mpsc_ring.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace chronoring::test {

namespace {

template <typename T, std::size_t Cap = 8>
concept CanInstantiateMpscRing = requires { typename MpscRingBuffer<T, Cap>; };

struct MpscNonDefault {
    explicit MpscNonDefault(int v) : value(v) {}
    int value;
};

struct MpscThrowingDtor {
    MpscThrowingDtor() = default;
    MpscThrowingDtor(MpscThrowingDtor&&) noexcept = default;
    MpscThrowingDtor& operator=(MpscThrowingDtor&&) noexcept = default;
    ~MpscThrowingDtor() noexcept(false) {}
};

struct MpscThrowingMoveAssign {
    MpscThrowingMoveAssign() = default;
    MpscThrowingMoveAssign(MpscThrowingMoveAssign&&) noexcept = default;
    MpscThrowingMoveAssign& operator=(MpscThrowingMoveAssign&&) noexcept(false) {
        return *this;
    }
};

struct NonTrivialThrowingDefaultMpsc {
    NonTrivialThrowingDefaultMpsc() noexcept(false) {}
    NonTrivialThrowingDefaultMpsc(NonTrivialThrowingDefaultMpsc&&) noexcept = default;
    NonTrivialThrowingDefaultMpsc& operator=(NonTrivialThrowingDefaultMpsc&&) noexcept = default;
    ~NonTrivialThrowingDefaultMpsc() noexcept {}
};

struct MaybeThrowingCtorItem {
    int id{0};
    bool poison_copy{false};

    MaybeThrowingCtorItem() noexcept = default;
    explicit MaybeThrowingCtorItem(int v, bool should_throw = false, bool throw_on_copy = false)
        : id(v), poison_copy(throw_on_copy) {
        if (should_throw) {
            throw std::runtime_error("simulated producer construction failure");
        }
    }
    MaybeThrowingCtorItem(const MaybeThrowingCtorItem& other)
        : id(other.id), poison_copy(other.poison_copy) {
        if (other.poison_copy) {
            throw std::runtime_error("simulated producer copy failure");
        }
    }
    MaybeThrowingCtorItem& operator=(const MaybeThrowingCtorItem&) = default;
    MaybeThrowingCtorItem(MaybeThrowingCtorItem&&) noexcept = default;
    MaybeThrowingCtorItem& operator=(MaybeThrowingCtorItem&&) noexcept = default;
    ~MaybeThrowingCtorItem() noexcept = default;
};

static_assert(CanInstantiateMpscRing<EventFrame, 16>);
static_assert(CanInstantiateMpscRing<std::unique_ptr<int>, 8>);
static_assert(CanInstantiateMpscRing<MaybeThrowingCtorItem, 8>);
static_assert(CanInstantiateMpscRing<NonTrivialThrowingDefaultMpsc, 8>);
static_assert(!CanInstantiateMpscRing<MpscNonDefault, 8>);
static_assert(!CanInstantiateMpscRing<MpscThrowingDtor, 8>);
static_assert(!CanInstantiateMpscRing<MpscThrowingMoveAssign, 8>);
static_assert(!CanInstantiateMpscRing<int, 6>);

static_assert(noexcept(std::declval<MpscRingBuffer<EventFrame, 8>&>().try_pop(
    std::declval<EventFrame&>())));
static_assert(noexcept(std::declval<MpscRingBuffer<EventFrame, 8>&>().try_pop_batch(nullptr, 0)));
static_assert(noexcept(std::declval<MpscRingBuffer<std::uint64_t, 8>&>().pop_wait(
    std::declval<std::uint64_t&>())));
static_assert(!noexcept(std::declval<MpscRingBuffer<NonTrivialThrowingDefaultMpsc, 8>&>().try_pop(
    std::declval<NonTrivialThrowingDefaultMpsc&>())));
static_assert(!noexcept(
    std::declval<MpscRingBuffer<NonTrivialThrowingDefaultMpsc, 8>&>().try_pop_batch(nullptr, 0)));
static_assert(!noexcept(std::declval<MpscRingBuffer<NonTrivialThrowingDefaultMpsc, 8>&>().pop_wait(
    std::declval<NonTrivialThrowingDefaultMpsc&>())));

} // namespace

TEST(MpscRingTest, SingleThreadedWrapAroundStatusAndSpanPop) {
    MpscRingBuffer<std::uint64_t, 8> ring;
    EXPECT_EQ(ring.capacity(), 8U);
    EXPECT_TRUE(ring.empty());

    for (std::uint64_t round = 0; round < 10; ++round) {
        for (std::uint64_t i = 0; i < 8; ++i) {
            EXPECT_EQ(ring.try_push_status(round * 100ULL + i), RingStatus::Ok);
        }
        EXPECT_EQ(ring.try_push_status(9999ULL), RingStatus::Full);
        EXPECT_EQ(ring.size_approx(), 8U);

        std::uint64_t out[4]{};
        EXPECT_EQ(ring.try_pop_batch(std::span<std::uint64_t>(out)), 4U);
        for (std::uint64_t i = 0; i < 4; ++i) {
            EXPECT_EQ(out[i], round * 100ULL + i);
        }
        for (std::uint64_t i = 4; i < 8; ++i) {
            std::uint64_t val = 0;
            EXPECT_EQ(ring.try_pop_status(val), RingStatus::Ok);
            EXPECT_EQ(val, round * 100ULL + i);
        }
        std::uint64_t extra = 0;
        EXPECT_EQ(ring.try_pop_status(extra), RingStatus::Empty);
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

TEST(MpscRingTest, BlockingPushPopWaitAndTimeout) {
    MpscRingBuffer<std::uint64_t, 4> ring;
    std::uint64_t tmp = 0;
    EXPECT_FALSE(ring.pop_wait(tmp, std::chrono::microseconds(200)));

    for (std::uint64_t i = 0; i < 4; ++i) {
        EXPECT_TRUE(ring.push_wait(i));
    }
    EXPECT_FALSE(ring.push_wait(999ULL, std::chrono::microseconds(200)));

    for (std::uint64_t i = 0; i < 4; ++i) {
        EXPECT_TRUE(ring.pop_wait(tmp));
        EXPECT_EQ(tmp, i);
    }

    constexpr std::uint64_t kCount = 2000;
    std::thread producer([&]() {
        for (std::uint64_t i = 0; i < kCount; ++i) {
            EXPECT_TRUE(ring.push_wait(i));
        }
    });

    std::thread consumer([&]() {
        std::uint64_t val = 0;
        for (std::uint64_t i = 0; i < kCount; ++i) {
            EXPECT_TRUE(ring.pop_wait(val));
            EXPECT_EQ(val, i);
        }
    });

    producer.join();
    consumer.join();
    EXPECT_TRUE(ring.empty());
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

TEST(MpscRingTest, ThrowingConstructorDoesNotStallConsumer) {
    MpscRingBuffer<MaybeThrowingCtorItem, 8> ring;

    EXPECT_TRUE(ring.try_emplace(10));
    EXPECT_THROW((void)ring.try_emplace(99, true), std::runtime_error);

    MaybeThrowingCtorItem copy_poison(77, false, true);
    EXPECT_THROW((void)ring.try_push(copy_poison), std::runtime_error);

    // Neither enqueue position nor slot sequences were corrupted or left stalled.
    EXPECT_EQ(ring.size_approx(), 1U);
    EXPECT_TRUE(ring.try_emplace(20));
    EXPECT_EQ(ring.size_approx(), 2U);

    MaybeThrowingCtorItem out;
    ASSERT_TRUE(ring.try_pop(out));
    EXPECT_EQ(out.id, 10);
    ASSERT_TRUE(ring.try_pop(out));
    EXPECT_EQ(out.id, 20);
    EXPECT_FALSE(ring.try_pop(out));
    EXPECT_TRUE(ring.empty());
}

} // namespace chronoring::test
