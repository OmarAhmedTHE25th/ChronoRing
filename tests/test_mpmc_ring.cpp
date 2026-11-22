#include "chronoring/event_frame.hpp"
#include "chronoring/mpmc_ring.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace chronoring::test {

namespace {

bool det_is_0(const EventFrame& f) noexcept { return f.detector_id == 0U; }
bool det_is_1(const EventFrame& f) noexcept { return f.detector_id == 1U; }
bool det_is_2(const EventFrame& f) noexcept { return f.detector_id == 2U; }

} // namespace

TEST(MpmcRingTest, FourProducersFourConsumersNoLossOrDuplicates) {
    constexpr std::size_t kProducers = 4;
    constexpr std::size_t kConsumers = 4;
    constexpr std::size_t kPerProducer = 10'000;
    constexpr std::size_t kTotal = kProducers * kPerProducer;

    auto ring = std::make_unique<MpmcRingBuffer<EventFrame, 1024>>();
    std::vector<std::atomic<std::uint8_t>> seen(kTotal);
    for (auto& s : seen) {
        s.store(0, std::memory_order_relaxed);
    }
    std::atomic<std::size_t> total_popped{0};
    std::atomic<bool> producers_done{false};

    std::vector<std::thread> prod_threads;
    prod_threads.reserve(kProducers);
    for (std::size_t p = 0; p < kProducers; ++p) {
        prod_threads.emplace_back([&, p]() {
            const std::uint64_t base = p * kPerProducer;
            for (std::size_t i = 0; i < kPerProducer; ++i) {
                EventFrame ev{};
                ev.sequence_id = base + i;
                ev.detector_id = static_cast<std::uint32_t>(p);
                while (ring->try_push_status(ev) != RingStatus::Ok) {
                    std::this_thread::yield();
                }
            }
        });
    }

    std::vector<std::thread> cons_threads;
    cons_threads.reserve(kConsumers);
    for (std::size_t c = 0; c < kConsumers; ++c) {
        cons_threads.emplace_back([&]() {
            EventFrame ev{};
            for (;;) {
                if (ring->try_pop_status(ev) == RingStatus::Ok) {
                    ASSERT_LT(ev.sequence_id, kTotal);
                    EXPECT_EQ(seen[ev.sequence_id].fetch_add(1, std::memory_order_relaxed), 0U);
                    total_popped.fetch_add(1, std::memory_order_relaxed);
                } else if (producers_done.load(std::memory_order_acquire) &&
                           total_popped.load(std::memory_order_acquire) >= kTotal) {
                    break;
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }

    for (auto& t : prod_threads) {
        t.join();
    }
    producers_done.store(true, std::memory_order_release);
    for (auto& t : cons_threads) {
        t.join();
    }

    EXPECT_EQ(total_popped.load(), kTotal);
    EXPECT_TRUE(ring->empty());
    for (std::size_t i = 0; i < kTotal; ++i) {
        EXPECT_EQ(seen[i].load(), 1U);
    }
}

TEST(MpmcRingTest, TopologyRouterBroadcastRoundRobinAndFiltered) {
    auto router = std::make_unique<RingTopologyRouter<EventFrame, 128, 3>>();

    // 1. Broadcast mode: every output receives all 30 items
    router->set_mode(RoutingMode::Broadcast);
    for (std::uint64_t i = 0; i < 30; ++i) {
        EventFrame ev{};
        ev.sequence_id = i;
        EXPECT_EQ(router->route_push(ev), 0x7ULL);
    }
    for (std::size_t out = 0; out < 3; ++out) {
        EXPECT_EQ(router->size_approx(out), 30U);
        EventFrame ev{};
        for (std::uint64_t i = 0; i < 30; ++i) {
            ASSERT_TRUE(router->try_pop(out, ev));
            EXPECT_EQ(ev.sequence_id, i);
        }
    }

    // 2. Round-Robin across 2 concurrent producers -> 30 each on 3 outputs
    router->set_mode(RoutingMode::RoundRobin);
    std::thread p1([&]() {
        for (std::uint64_t i = 0; i < 45; ++i) {
            EventFrame ev{};
            ev.sequence_id = i;
            EXPECT_NE(router->route_push(ev), 0ULL);
        }
    });
    std::thread p2([&]() {
        for (std::uint64_t i = 45; i < 90; ++i) {
            EventFrame ev{};
            ev.sequence_id = i;
            EXPECT_NE(router->route_push(ev), 0ULL);
        }
    });
    p1.join();
    p2.join();
    EXPECT_EQ(router->size_approx(0), 30U);
    EXPECT_EQ(router->size_approx(1), 30U);
    EXPECT_EQ(router->size_approx(2), 30U);

    EventFrame drain{};
    for (std::size_t out = 0; out < 3; ++out) {
        while (router->try_pop(out, drain)) {}
    }

    // 3. PredicateFiltered mode routing by detector_id
    router->set_mode(RoutingMode::PredicateFiltered);
    router->set_output_predicate(0, &det_is_0);
    router->set_output_predicate(1, &det_is_1);
    router->set_output_predicate(2, &det_is_2);

    for (std::uint64_t i = 0; i < 30; ++i) {
        EventFrame ev{};
        ev.sequence_id = i;
        ev.detector_id = static_cast<std::uint32_t>(i % 3U);
        EXPECT_EQ(router->route_push(ev), std::uint64_t{1} << (i % 3U));
    }
    EXPECT_EQ(router->size_approx(0), 10U);
    EXPECT_EQ(router->size_approx(1), 10U);
    EXPECT_EQ(router->size_approx(2), 10U);
    for (std::size_t out = 0; out < 3; ++out) {
        EventFrame ev{};
        while (router->try_pop(out, ev)) {
            EXPECT_EQ(ev.detector_id, out);
        }
    }
}

} // namespace chronoring::test
