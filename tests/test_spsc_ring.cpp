#include "chronoring/c_api.h"
#include "chronoring/event_frame.hpp"
#include "chronoring/spsc_ring.hpp"
#include "chronoring/status.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace chronoring::test {

namespace {

template <typename T, std::size_t Cap = 8>
concept CanInstantiateSpscRing = requires { typename SpscRingBuffer<T, Cap>; };

struct NonDefaultConstructible {
    explicit NonDefaultConstructible(int v) : value(v) {}
    int value;
};

struct NonMovable {
    NonMovable() = default;
    NonMovable(const NonMovable&) = delete;
    NonMovable& operator=(const NonMovable&) = delete;
    NonMovable(NonMovable&&) = delete;
    NonMovable& operator=(NonMovable&&) = delete;
};

struct ThrowingDestructor {
    ThrowingDestructor() = default;
    ThrowingDestructor(ThrowingDestructor&&) = default;
    ThrowingDestructor& operator=(ThrowingDestructor&&) = default;
    ~ThrowingDestructor() noexcept(false) {}
};

static_assert(CanInstantiateSpscRing<int, 8>);
static_assert(CanInstantiateSpscRing<EventFrame, 16>);
static_assert(CanInstantiateSpscRing<std::unique_ptr<int>, 8>);
static_assert(!CanInstantiateSpscRing<NonDefaultConstructible, 8>);
static_assert(!CanInstantiateSpscRing<NonMovable, 8>);
static_assert(!CanInstantiateSpscRing<ThrowingDestructor, 8>);
static_assert(!CanInstantiateSpscRing<int, 7>);

EventFrame make_frame(std::uint64_t seq) noexcept {
    EventFrame f{};
    f.timestamp_ns = 1000ULL + seq * 10ULL;
    f.sequence_id = seq;
    f.detector_id = static_cast<std::uint32_t>(seq % 8U);
    f.channel_id = static_cast<std::uint32_t>(seq % 64U);
    f.energy_mev = static_cast<float>(seq % 1000U) + 0.5f;
    f.quality_flags = kFlagValid;
    f.multiplicity = 1;
    return f;
}

template <std::size_t Cap>
void run_capacity_stress(std::size_t total_events) {
    auto ring = std::make_unique<SpscRingBuffer<EventFrame, Cap>>();
    std::thread producer([&]() {
        for (std::size_t i = 0; i < total_events; ++i) {
            const EventFrame f = make_frame(i);
            while (!ring->try_push(f)) {
                std::this_thread::yield();
            }
        }
    });

    std::size_t received = 0;
    std::thread consumer([&]() {
        EventFrame f{};
        while (received < total_events) {
            if (ring->try_pop(f)) {
                EXPECT_EQ(f.sequence_id, received);
                EXPECT_EQ(f.timestamp_ns, 1000ULL + received * 10ULL);
                ++received;
            } else {
                std::this_thread::yield();
            }
        }
    });
    producer.join();
    consumer.join();
    EXPECT_EQ(received, total_events);
    EXPECT_TRUE(ring->empty());
}

} // namespace

TEST(SpscRingTest, StatusExpectedAndStructuredLogger) {
    EXPECT_EQ(version(), CHRONORING_VERSION_STRING);
    EXPECT_EQ(status_string(RingStatus::Ok), "Ok");
    EXPECT_EQ(status_string(RingStatus::CorruptedPage), "CorruptedPage");

    Expected<int, RingStatus> ok_val(42);
    EXPECT_TRUE(ok_val.has_value());
    EXPECT_EQ(*ok_val, 42);
    EXPECT_EQ(ok_val.value_or(0), 42);

    Expected<int, RingStatus> err_val = unexpected(RingStatus::Full);
    EXPECT_FALSE(err_val.has_value());
    EXPECT_EQ(err_val.error(), RingStatus::Full);
    EXPECT_EQ(err_val.value_or(-1), -1);

    Expected<void, RingStatus> void_err = unexpected(RingStatus::IoError);
    EXPECT_FALSE(void_err.has_value());
    EXPECT_EQ(void_err.error(), RingStatus::IoError);

    struct LogCapture {
        std::atomic<int> count{0};
        RingStatus last_status{RingStatus::Ok};
        std::uint64_t last_metric{0};
    } cap;

    auto cb = +[](const LogEvent& ev, void* ctx) noexcept {
        auto* c = static_cast<LogCapture*>(ctx);
        c->last_status = ev.status;
        c->last_metric = ev.metric_value;
        c->count.fetch_add(1, std::memory_order_relaxed);
    };
    set_logger(cb, &cap);
    emit_log(LogLevel::Warn, RingStatus::Full, "test", "ring full", 123);
    set_logger(nullptr, nullptr);

    EXPECT_EQ(cap.count.load(), 1);
    EXPECT_EQ(cap.last_status, RingStatus::Full);
    EXPECT_EQ(cap.last_metric, 123U);
}

TEST(SpscRingTest, SingleThreadedPushPopCapacityAndStatus) {
    SpscRingBuffer<int, 8> ring;
    EXPECT_EQ(ring.capacity(), 8U);
    EXPECT_TRUE(ring.empty());
    EXPECT_EQ(ring.size_approx(), 0U);

    for (int i = 0; i < 8; ++i) {
        EXPECT_EQ(ring.try_push_status(i * 10), RingStatus::Ok);
    }
    EXPECT_EQ(ring.try_push_status(999), RingStatus::Full);
    EXPECT_EQ(ring.size_approx(), 8U);

    for (int i = 0; i < 8; ++i) {
        int val = -1;
        EXPECT_EQ(ring.try_pop_status(val), RingStatus::Ok);
        EXPECT_EQ(val, i * 10);
    }
    int extra = 0;
    EXPECT_EQ(ring.try_pop_status(extra), RingStatus::Empty);
    EXPECT_TRUE(ring.empty());
}

TEST(SpscRingTest, ZeroCopyReservePeekAndSpanBatch) {
    SpscRingBuffer<EventFrame, 16> ring;

    for (std::uint64_t i = 0; i < 5; ++i) {
        EventFrame* slot = ring.try_reserve_push();
        ASSERT_NE(slot, nullptr);
        *slot = make_frame(i);
        ring.commit_push();
    }
    EXPECT_EQ(ring.size_approx(), 5U);

    for (std::uint64_t i = 0; i < 5; ++i) {
        const EventFrame* peeked = ring.try_peek();
        ASSERT_NE(peeked, nullptr);
        EXPECT_EQ(peeked->sequence_id, i);
        ring.consume_pop();
    }
    EXPECT_EQ(ring.try_peek(), nullptr);

    std::array<EventFrame, 10> in{};
    std::array<EventFrame, 10> out{};
    for (std::size_t round = 0; round < 30; ++round) {
        for (std::size_t i = 0; i < in.size(); ++i) {
            in[i] = make_frame(round * 10U + i);
        }
        EXPECT_EQ(ring.try_push_batch(std::span<const EventFrame>(in)), 10U);
        EXPECT_EQ(ring.try_pop_batch(std::span<EventFrame>(out)), 10U);
        for (std::size_t i = 0; i < out.size(); ++i) {
            EXPECT_EQ(out[i].sequence_id, round * 10U + i);
        }
    }
}

TEST(SpscRingTest, NonTrivialSlotResetOnPop) {
    SpscRingBuffer<std::shared_ptr<int>, 4> ring;
    auto ptr = std::make_shared<int>(99);
    EXPECT_EQ(ptr.use_count(), 1);
    EXPECT_TRUE(ring.try_push(ptr));
    EXPECT_EQ(ptr.use_count(), 2);

    std::shared_ptr<int> out;
    EXPECT_TRUE(ring.try_pop(out));
    EXPECT_EQ(*out, 99);
    out.reset();
    // Ring slot must have released its reference upon pop
    EXPECT_EQ(ptr.use_count(), 1);
}

TEST(SpscRingTest, FanOutBroadcastToMultipleConsumers) {
    constexpr std::size_t kItems = 10'000;
    auto fanout = std::make_unique<SpscFanOut<EventFrame, 256, 3>>();

    std::thread producer([&]() {
        for (std::size_t i = 0; i < kItems; ++i) {
            const EventFrame ev = make_frame(i);
            while (!fanout->broadcast_all(ev)) {
                std::this_thread::yield();
            }
        }
    });

    std::array<std::thread, 3> consumers;
    for (std::size_t c = 0; c < 3; ++c) {
        consumers[c] = std::thread([&, c]() {
            std::size_t got = 0;
            EventFrame ev{};
            while (got < kItems) {
                if (fanout->try_pop(c, ev)) {
                    EXPECT_EQ(ev.sequence_id, got);
                    ++got;
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }

    producer.join();
    for (auto& th : consumers) {
        th.join();
    }
}

TEST(SpscRingTest, MultiCapacityStressOrderingAndIntegrity) {
    run_capacity_stress<8>(5'000);
    run_capacity_stress<64>(20'000);
    run_capacity_stress<1024>(100'000);
}

TEST(SpscRingTest, CApiSurfaceSmoke) {
    EXPECT_STREQ(chronoring_version(), CHRONORING_VERSION_STRING);
    constexpr char kVec[] = "123456789";
    EXPECT_EQ(chronoring_crc32c(kVec, 9, 0), 0xE3069283u);

    chronoring_spsc_t* ring = chronoring_spsc_create();
    ASSERT_NE(ring, nullptr);

    chronoring_event_frame_t in[2]{};
    in[0].sequence_id = 10;
    in[0].energy_mev = 50.0f;
    in[0].quality_flags = kFlagValid;
    in[0].multiplicity = 2;

    in[1].sequence_id = 11;
    in[1].energy_mev = 0.01f; // fails min energy
    in[1].quality_flags = kFlagValid;
    in[1].multiplicity = 2;

    EXPECT_EQ(chronoring_spsc_try_push(ring, &in[0]), 1);
    EXPECT_EQ(chronoring_spsc_size(ring), 1U);

    chronoring_event_frame_t popped{};
    EXPECT_EQ(chronoring_spsc_try_pop(ring, &popped), 1);
    EXPECT_EQ(popped.sequence_id, 10U);
    EXPECT_EQ(chronoring_spsc_try_pop(ring, &popped), 0);

    const chronoring_trigger_config_t cfg = chronoring_default_trigger_config();
    chronoring_event_frame_t filtered[2]{};
    EXPECT_EQ(chronoring_filter_events(in, 2, &cfg, filtered), 1U);
    EXPECT_EQ(filtered[0].sequence_id, 10U);

    chronoring_spsc_destroy(ring);
}

TEST(SpscRingTest, RaiiWriteAndReadReservations) {
    SpscRingBuffer<std::unique_ptr<int>, 4> ring;

    // 1. Auto-commit on WriteReservation destruction
    {
        auto write_guard = ring.reserve_push();
        ASSERT_TRUE(write_guard);
        *write_guard = std::make_unique<int>(101);
        EXPECT_EQ(ring.size_approx(), 0U);
    }
    EXPECT_EQ(ring.size_approx(), 1U);

    // 2. Explicit idempotent commit + move transfer
    {
        auto g1 = ring.reserve_push();
        ASSERT_TRUE(g1);
        *g1 = std::make_unique<int>(202);
        auto g2 = std::move(g1);
        EXPECT_FALSE(g1);
        ASSERT_TRUE(g2);
        g2.commit();
        g2.commit(); // idempotent no-op
    }
    EXPECT_EQ(ring.size_approx(), 2U);

    // 3. Move-assignment commits previously held write reservation
    {
        auto g1 = ring.reserve_push();
        ASSERT_TRUE(g1);
        *g1 = std::make_unique<int>(303);
        auto g2 = std::move(g1);

        // Push a third slot via auto-committed move-assignment target
        g1 = std::move(g2);
        EXPECT_FALSE(g2);
        ASSERT_TRUE(g1);
    }
    EXPECT_EQ(ring.size_approx(), 3U);

    // 4. Fill final slot and check full reservation returns empty guard
    {
        auto g4 = ring.reserve_push();
        ASSERT_TRUE(g4);
        *g4 = std::make_unique<int>(404);
    }
    EXPECT_EQ(ring.size_approx(), 4U);
    EXPECT_FALSE(ring.reserve_push());

    // 5. ReadReservation auto-consume, move, and idempotent commit
    {
        auto read_guard = ring.reserve_pop();
        ASSERT_TRUE(read_guard);
        EXPECT_EQ(**read_guard, 101);
        auto moved_read = std::move(read_guard);
        EXPECT_FALSE(read_guard);
        moved_read.commit();
        moved_read.commit();
    }
    EXPECT_EQ(ring.size_approx(), 3U);

    for (int expected : {202, 303, 404}) {
        auto r = ring.reserve_pop();
        ASSERT_TRUE(r);
        EXPECT_EQ(**r, expected);
    }
    EXPECT_TRUE(ring.empty());
    EXPECT_FALSE(ring.reserve_pop());
}

#ifndef NDEBUG
TEST(SpscRingDeathTest, CommitWithoutReservationAndDoubleCommitAbort) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    using Ring4 = SpscRingBuffer<int, 4>;

    EXPECT_DEATH(
        {
            Ring4 ring;
            ring.commit_push();
        },
        "commit_push\\(\\) called without an active write reservation");

    EXPECT_DEATH(
        {
            Ring4 ring;
            int* slot = ring.try_reserve_push();
            if (slot != nullptr) {
                *slot = 7;
                ring.commit_push();
                ring.commit_push();
            }
        },
        "commit_push\\(\\) called without an active write reservation");

    EXPECT_DEATH(
        {
            Ring4 ring;
            (void)ring.try_reserve_push();
            (void)ring.try_reserve_push();
        },
        "try_reserve_push\\(\\) called while a write reservation is already active");

    EXPECT_DEATH(
        {
            Ring4 ring;
            (void)ring.try_push(1);
            ring.consume_pop();
        },
        "consume_pop\\(\\) called without an active read reservation");

    EXPECT_DEATH(
        {
            Ring4 ring;
            (void)ring.try_push(1);
            (void)ring.try_peek();
            ring.consume_pop();
            ring.consume_pop();
        },
        "consume_pop\\(\\) called without an active read reservation");
}
#endif

} // namespace chronoring::test
