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
#include <stdexcept>
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

struct NonTrivialThrowingDefault {
    NonTrivialThrowingDefault() noexcept(false) {}
    NonTrivialThrowingDefault(NonTrivialThrowingDefault&&) noexcept = default;
    NonTrivialThrowingDefault& operator=(NonTrivialThrowingDefault&&) noexcept = default;
    ~NonTrivialThrowingDefault() noexcept {}
};

struct ResetThrowTracker {
    static inline bool throw_next_default = false;
    static inline int default_calls_before_throw = -1;

    int value{0};
    bool moved_from{false};

    ResetThrowTracker() {
        if (default_calls_before_throw == 0 || throw_next_default) {
            throw_next_default = false;
            default_calls_before_throw = -1;
            throw std::runtime_error("simulated default construction failure on slot reset");
        }
        if (default_calls_before_throw > 0) {
            --default_calls_before_throw;
        }
    }

    explicit ResetThrowTracker(int v) noexcept : value(v), moved_from(false) {}

    ResetThrowTracker(ResetThrowTracker&& other) noexcept
        : value(other.value), moved_from(false) {
        other.value = -1;
        other.moved_from = true;
    }

    ResetThrowTracker& operator=(ResetThrowTracker&& other) noexcept {
        if (this != &other) {
            value = other.value;
            moved_from = false;
            other.value = -1;
            other.moved_from = true;
        }
        return *this;
    }

    ~ResetThrowTracker() noexcept {}
};

static_assert(CanInstantiateSpscRing<int, 8>);
static_assert(CanInstantiateSpscRing<EventFrame, 16>);
static_assert(CanInstantiateSpscRing<std::unique_ptr<int>, 8>);
static_assert(CanInstantiateSpscRing<NonTrivialThrowingDefault, 8>);
static_assert(!CanInstantiateSpscRing<NonDefaultConstructible, 8>);
static_assert(!CanInstantiateSpscRing<NonMovable, 8>);
static_assert(!CanInstantiateSpscRing<ThrowingDestructor, 8>);
static_assert(!CanInstantiateSpscRing<int, 7>);

static_assert(noexcept(std::declval<SpscRingBuffer<EventFrame, 8>&>().consume_pop()));
static_assert(noexcept(std::declval<SpscRingBuffer<EventFrame, 8>&>().try_pop(
    std::declval<EventFrame&>())));
static_assert(noexcept(std::declval<SpscRingBuffer<EventFrame, 8>&>().try_pop_batch(nullptr, 0)));
static_assert(noexcept(
    std::declval<SpscRingBuffer<EventFrame, 8>::ReadReservation&>().commit()));

static_assert(!noexcept(
    std::declval<SpscRingBuffer<NonTrivialThrowingDefault, 8>&>().consume_pop()));
static_assert(!noexcept(std::declval<SpscRingBuffer<NonTrivialThrowingDefault, 8>&>().try_pop(
    std::declval<NonTrivialThrowingDefault&>())));
static_assert(!noexcept(
    std::declval<SpscRingBuffer<NonTrivialThrowingDefault, 8>&>().try_pop_batch(nullptr, 0)));
static_assert(!noexcept(
    std::declval<SpscRingBuffer<NonTrivialThrowingDefault, 8>::ReadReservation&>().commit()));

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

    // 1. Dropping an uncommitted WriteReservation cancels cleanly without publishing
    {
        auto uncommitted = ring.reserve_push();
        ASSERT_TRUE(uncommitted);
        *uncommitted = std::make_unique<int>(999);
        EXPECT_EQ(ring.size_approx(), 0U);
    }
    EXPECT_TRUE(ring.empty());

    // 2. Explicit commit + idempotency + move transfer
    {
        auto g1 = ring.reserve_push();
        ASSERT_TRUE(g1);
        *g1 = std::make_unique<int>(101);
        auto g2 = std::move(g1);
        EXPECT_FALSE(g1);
        ASSERT_TRUE(g2);
        EXPECT_EQ(ring.size_approx(), 0U);
        g2.commit();
        g2.commit(); // idempotent no-op
    }
    EXPECT_EQ(ring.size_approx(), 1U);

    // 3. Move-assignment cancels any currently held uncommitted reservation before moving
    {
        auto held_uncommitted = ring.reserve_push();
        ASSERT_TRUE(held_uncommitted);
        *held_uncommitted = std::make_unique<int>(888);

        // Move-assign an empty guard: cancels `held_uncommitted` without publishing
        held_uncommitted = SpscRingBuffer<std::unique_ptr<int>, 4>::WriteReservation{};
        EXPECT_FALSE(held_uncommitted);
        EXPECT_EQ(ring.size_approx(), 1U);

        auto g2 = ring.reserve_push();
        ASSERT_TRUE(g2);
        *g2 = std::make_unique<int>(202);
        g2.commit();
    }
    EXPECT_EQ(ring.size_approx(), 2U);

    for (int v : {303, 404}) {
        auto g = ring.reserve_push();
        ASSERT_TRUE(g);
        *g = std::make_unique<int>(v);
        g.commit();
    }
    EXPECT_EQ(ring.size_approx(), 4U);
    EXPECT_FALSE(ring.reserve_push());

    // 4. Dropping an uncommitted ReadReservation cancels without consuming the item
    {
        auto read_uncommitted = ring.reserve_pop();
        ASSERT_TRUE(read_uncommitted);
        EXPECT_EQ(**read_uncommitted, 101);
    }
    EXPECT_EQ(ring.size_approx(), 4U);

    // 5. Explicit ReadReservation commit & move semantics
    {
        auto r1 = ring.reserve_pop();
        ASSERT_TRUE(r1);
        EXPECT_EQ(**r1, 101);
        auto r2 = std::move(r1);
        EXPECT_FALSE(r1);
        r2.commit();
        r2.commit();
    }
    EXPECT_EQ(ring.size_approx(), 3U);

    for (int expected : {202, 303, 404}) {
        auto r = ring.reserve_pop();
        ASSERT_TRUE(r);
        EXPECT_EQ(**r, expected);
        r.commit();
    }
    EXPECT_TRUE(ring.empty());
    EXPECT_FALSE(ring.reserve_pop());
}

TEST(SpscRingTest, ExceptionDuringWriteReservationCancelsCleanly) {
    SpscRingBuffer<EventFrame, 4> ring;

    EXPECT_THROW(
        {
            auto guard = ring.reserve_push();
            EXPECT_TRUE(guard);
            guard->sequence_id = 777U;
            guard->energy_mev = 123.0f;
            throw std::runtime_error("simulated populate failure");
        },
        std::runtime_error);

    // Partial item was NOT committed on stack unwind, and Debug reservation state was cleared
    EXPECT_TRUE(ring.empty());
    EXPECT_FALSE(ring.reserve_pop());

    // Subsequent reservation & explicit commit succeed normally
    auto next = ring.reserve_push();
    ASSERT_TRUE(next);
    *next = make_frame(888U);
    next.commit();
    EXPECT_EQ(ring.size_approx(), 1U);

    auto read = ring.reserve_pop();
    ASSERT_TRUE(read);
    EXPECT_EQ(read->sequence_id, 888U);
    read.commit();
    EXPECT_TRUE(ring.empty());
}

TEST(SpscRingTest, StrongExceptionSafetyWhenSlotResetThrows) {
    ResetThrowTracker::throw_next_default = false;
    ResetThrowTracker::default_calls_before_throw = -1;

    SpscRingBuffer<ResetThrowTracker, 8> ring;
    ASSERT_TRUE(ring.try_emplace(42));

    ResetThrowTracker out(-99);
    ResetThrowTracker::throw_next_default = true;
    EXPECT_THROW((void)ring.try_pop(out), std::runtime_error);

    // Slot was NOT moved from, `out` is unchanged, and tail did not advance.
    EXPECT_EQ(out.value, -99);
    EXPECT_EQ(ring.size_approx(), 1U);

    ASSERT_TRUE(ring.try_pop(out));
    EXPECT_EQ(out.value, 42);
    EXPECT_FALSE(out.moved_from);
    EXPECT_TRUE(ring.empty());

    // Batch pop: second item throws on slot reset -> first item succeeds and commits,
    // second item remains intact in the ring for a retry.
    ASSERT_TRUE(ring.try_emplace(111));
    ASSERT_TRUE(ring.try_emplace(222));

    std::array<ResetThrowTracker, 2> batch_out{
        ResetThrowTracker(-99), ResetThrowTracker(-99)};
    ResetThrowTracker::default_calls_before_throw = 1;
    EXPECT_THROW((void)ring.try_pop_batch(batch_out.data(), 2), std::runtime_error);

    EXPECT_EQ(batch_out[0].value, 111);
    EXPECT_EQ(batch_out[1].value, -99);
    EXPECT_EQ(ring.size_approx(), 1U);

    ResetThrowTracker retry(-99);
    ASSERT_TRUE(ring.try_pop(retry));
    EXPECT_EQ(retry.value, 222);
    EXPECT_FALSE(retry.moved_from);
    EXPECT_TRUE(ring.empty());

    ResetThrowTracker::throw_next_default = false;
    ResetThrowTracker::default_calls_before_throw = -1;
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
