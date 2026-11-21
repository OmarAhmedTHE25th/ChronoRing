#include "chronoring/event_frame.hpp"
#include "chronoring/shm_ring.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace chronoring::test {

namespace {

std::string unique_shm_name(const char* suffix) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::string("chronoring_test_") + suffix + "_" + std::to_string(now);
}

EventFrame make_ipc_frame(std::uint64_t seq) noexcept {
    EventFrame f{};
    f.timestamp_ns = 5'000'000ULL + seq * 25ULL;
    f.sequence_id = seq;
    f.detector_id = static_cast<std::uint32_t>(seq % 12U);
    f.channel_id = static_cast<std::uint32_t>(seq % 128U);
    f.energy_mev = 12.5f + static_cast<float>(seq % 100U);
    f.quality_flags = kFlagValid | kFlagCalibrated;
    f.multiplicity = 2;
    return f;
}

} // namespace

TEST(ShmRingTest, OpenNonExistentFailsGracefully) {
    const std::string name = unique_shm_name("missing");
    (void)ShmRingBuffer::unlink(name);
    ShmRingBuffer ring(name, ShmMode::Open);
    EXPECT_FALSE(ring.is_open());
    EXPECT_FALSE(ring.last_error().empty());
}

TEST(ShmRingTest, DualMappingWrapAroundAndClose) {
    const std::string name = unique_shm_name("dual");
    ShmRingBuffer producer(name, ShmMode::Create, 64, false);
    ASSERT_TRUE(producer.is_open()) << producer.last_error();

    ShmRingBuffer consumer(name, ShmMode::Open);
    ASSERT_TRUE(consumer.is_open()) << consumer.last_error();
    EXPECT_EQ(consumer.capacity(), 64U);

    for (std::uint64_t round = 0; round < 10; ++round) {
        std::vector<EventFrame> batch(32);
        for (std::size_t i = 0; i < batch.size(); ++i) {
            batch[i] = make_ipc_frame(round * 32ULL + i);
        }
        EXPECT_EQ(producer.try_push_batch(batch.data(), batch.size()), 32U);

        std::vector<EventFrame> out(32);
        EXPECT_EQ(consumer.try_pop_batch(out.data(), out.size()), 32U);
        for (std::size_t i = 0; i < out.size(); ++i) {
            EXPECT_EQ(out[i].sequence_id, round * 32ULL + i);
            EXPECT_FLOAT_EQ(out[i].energy_mev, batch[i].energy_mev);
        }
    }

    EXPECT_FALSE(consumer.is_closed());
    producer.close();
    EXPECT_TRUE(consumer.is_closed());

    consumer.unmap();
    producer.unmap();
    EXPECT_TRUE(ShmRingBuffer::unlink(name));
}

TEST(ShmRingTest, ConcurrentThreadsAcrossMappings) {
    const std::string name = unique_shm_name("threads");
    constexpr std::size_t kCount = 50'000;

    ShmRingBuffer producer(name, ShmMode::Create, 1024, false);
    ASSERT_TRUE(producer.is_open()) << producer.last_error();

    ShmRingBuffer consumer(name, ShmMode::Open);
    ASSERT_TRUE(consumer.is_open()) << consumer.last_error();

    std::thread prod_th([&]() {
        for (std::size_t i = 0; i < kCount; ++i) {
            const EventFrame ev = make_ipc_frame(i);
            while (!producer.try_push(ev)) {
                std::this_thread::yield();
            }
        }
        producer.close();
    });

    std::size_t received = 0;
    std::thread cons_th([&]() {
        EventFrame ev{};
        while (received < kCount) {
            if (consumer.try_pop(ev)) {
                EXPECT_EQ(ev.sequence_id, received);
                EXPECT_EQ(ev.timestamp_ns, 5'000'000ULL + received * 25ULL);
                ++received;
            } else if (consumer.is_closed() && consumer.empty()) {
                break;
            } else {
                std::this_thread::yield();
            }
        }
    });

    prod_th.join();
    cons_th.join();
    EXPECT_EQ(received, kCount);

    consumer.unmap();
    producer.unmap();
    (void)ShmRingBuffer::unlink(name);
}

#ifndef _WIN32
TEST(ShmRingTest, CrossProcessForkIpc) {
    const std::string name = unique_shm_name("fork");
    constexpr std::size_t kCount = 25'000;

    ShmRingBuffer producer(name, ShmMode::Create, 1024, false);
    ASSERT_TRUE(producer.is_open()) << producer.last_error();

    const pid_t pid = ::fork();
    ASSERT_NE(pid, -1);

    if (pid == 0) {
        producer.unmap();
        ShmRingBuffer child_cons(name, ShmMode::Open);
        if (!child_cons.is_open()) {
            _exit(2);
        }
        std::size_t got = 0;
        EventFrame ev{};
        while (got < kCount) {
            if (child_cons.try_pop(ev)) {
                if (ev.sequence_id != got) {
                    _exit(3);
                }
                ++got;
            } else if (child_cons.is_closed() && child_cons.empty()) {
                break;
            } else {
                std::this_thread::yield();
            }
        }
        child_cons.unmap();
        _exit(got == kCount ? 0 : 4);
    }

    for (std::size_t i = 0; i < kCount; ++i) {
        const EventFrame ev = make_ipc_frame(i);
        while (!producer.try_push(ev)) {
            std::this_thread::yield();
        }
    }
    producer.close();

    int status = 0;
    ASSERT_EQ(::waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);

    producer.unmap();
    (void)ShmRingBuffer::unlink(name);
}
#endif

} // namespace chronoring::test
