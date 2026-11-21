#include "chronoring/event_frame.hpp"
#include "chronoring/spsc_ring.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace chronoring::test {

namespace {

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

} // namespace

TEST(SpscRingTest, SingleThreadedPushPopCapacity) {
    SpscRingBuffer<int, 8> ring;
    EXPECT_EQ(ring.capacity(), 8U);
    EXPECT_TRUE(ring.empty());
    EXPECT_EQ(ring.size_approx(), 0U);

    for (int i = 0; i < 8; ++i) {
        EXPECT_TRUE(ring.try_push(i * 10));
    }
    EXPECT_FALSE(ring.try_push(999));
    EXPECT_EQ(ring.size_approx(), 8U);

    for (int i = 0; i < 8; ++i) {
        int val = -1;
        EXPECT_TRUE(ring.try_pop(val));
        EXPECT_EQ(val, i * 10);
    }
    int extra = 0;
    EXPECT_FALSE(ring.try_pop(extra));
    EXPECT_TRUE(ring.empty());
}

TEST(SpscRingTest, WrapAroundAndBatchOperations) {
    SpscRingBuffer<EventFrame, 16> ring;
    std::array<EventFrame, 10> in{};
    std::array<EventFrame, 10> out{};

    for (std::size_t round = 0; round < 50; ++round) {
        for (std::size_t i = 0; i < in.size(); ++i) {
            in[i] = make_frame(round * 10U + i);
        }
        EXPECT_EQ(ring.try_push_batch(in.data(), in.size()), 10U);
        EXPECT_EQ(ring.size_approx(), 10U);

        EXPECT_EQ(ring.try_pop_batch(out.data(), out.size()), 10U);
        EXPECT_TRUE(ring.empty());

        for (std::size_t i = 0; i < out.size(); ++i) {
            EXPECT_EQ(out[i].sequence_id, round * 10U + i);
            EXPECT_FLOAT_EQ(out[i].energy_mev, in[i].energy_mev);
        }
    }

    EXPECT_TRUE(ring.try_emplace(EventFrame{.timestamp_ns = 777, .sequence_id = 42}));
    EventFrame popped{};
    EXPECT_TRUE(ring.try_pop(popped));
    EXPECT_EQ(popped.timestamp_ns, 777U);
    EXPECT_EQ(popped.sequence_id, 42U);
}

TEST(SpscRingTest, MultiThreadedStressOrderingAndIntegrity) {
    constexpr std::size_t kTotalEvents = 250'000;
    auto ring = std::make_unique<SpscRingBuffer<EventFrame, 4096>>();

    std::thread producer([&]() {
        for (std::size_t i = 0; i < kTotalEvents; ++i) {
            const EventFrame f = make_frame(i);
            while (!ring->try_push(f)) {
                std::this_thread::yield();
            }
        }
    });

    std::size_t received = 0;
    std::thread consumer([&]() {
        EventFrame f{};
        while (received < kTotalEvents) {
            if (ring->try_pop(f)) {
                EXPECT_EQ(f.sequence_id, received);
                EXPECT_EQ(f.timestamp_ns, 1000ULL + received * 10ULL);
                EXPECT_EQ(f.detector_id, static_cast<std::uint32_t>(received % 8U));
                ++received;
            } else {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();
    EXPECT_EQ(received, kTotalEvents);
    EXPECT_TRUE(ring->empty());
}

} // namespace chronoring::test
