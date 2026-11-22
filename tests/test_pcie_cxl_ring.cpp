#include "chronoring/event_frame.hpp"
#include "chronoring/pcie_cxl_ring.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <span>
#include <thread>
#include <vector>

namespace chronoring::test {

namespace {

EventFrame make_dma_frame(std::uint64_t seq) noexcept {
    EventFrame f{};
    f.timestamp_ns = 1'000'000ULL + seq * 10ULL;
    f.sequence_id = seq;
    f.detector_id = static_cast<std::uint32_t>(seq % 8U);
    f.channel_id = static_cast<std::uint32_t>(seq % 256U);
    f.energy_mev = 20.0f + static_cast<float>(seq % 50U);
    f.quality_flags = kFlagValid | kFlagCalibrated;
    f.multiplicity = 1;
    return f;
}

} // namespace

TEST(PcieCxlRingTest, AllocatePinnedAndZeroCpuFpgaBurst) {
    constexpr std::size_t kCapacity = 64;
    constexpr std::uint64_t kBusAddr = 0xDEADBEEFCAFE0000ULL;

    PcieCxlRing host_ring;
    ASSERT_EQ(host_ring.allocate_pinned(kCapacity, kBusAddr), RingStatus::Ok);
    ASSERT_TRUE(host_ring.is_open());
    EXPECT_EQ(host_ring.capacity(), kCapacity);
    EXPECT_EQ(host_ring.transport_kind(), DmaTransportKind::HostPinned);
    EXPECT_EQ(host_ring.dma_bus_addr(), kBusAddr);
    EXPECT_FALSE(host_ring.is_device_closed());

    MockFpgaDmaProducer fpga(host_ring.raw_base());
    ASSERT_TRUE(fpga.is_ready());

    // Wrap around several times using burst DMA writes and zero-copy peek/consume + batch pop
    std::uint64_t next_seq = 0;
    for (int round = 0; round < 8; ++round) {
        std::vector<EventFrame> burst(32);
        for (std::size_t i = 0; i < burst.size(); ++i) {
            burst[i] = make_dma_frame(next_seq++);
        }
        EXPECT_EQ(fpga.dma_write_burst(std::span<const EventFrame>(burst)), 32U);
        EXPECT_EQ(host_ring.size_approx(), 32U);

        // Zero-copy peek/consume first item
        const EventFrame* peeked = host_ring.try_peek();
        ASSERT_NE(peeked, nullptr);
        EXPECT_EQ(peeked->sequence_id, burst[0].sequence_id);
        host_ring.consume_pop();

        // Batch pop remaining 31 items
        std::vector<EventFrame> out(31);
        EXPECT_EQ(host_ring.try_pop_batch(std::span<EventFrame>(out)), 31U);
        for (std::size_t i = 0; i < out.size(); ++i) {
            EXPECT_EQ(out[i].sequence_id, burst[i + 1].sequence_id);
            EXPECT_FLOAT_EQ(out[i].energy_mev, burst[i + 1].energy_mev);
        }
        EXPECT_TRUE(host_ring.empty());
    }

    fpga.signal_device_close();
    EXPECT_TRUE(host_ring.is_device_closed());
    EventFrame dummy{};
    EXPECT_EQ(host_ring.try_pop_status(dummy), RingStatus::Closed);
}

TEST(PcieCxlRingTest, ConcurrentFpgaProducerAndHostConsumer) {
    constexpr std::size_t kCapacity = 1024;
    constexpr std::size_t kTotalFrames = 50'000;

    PcieCxlRing host_ring;
    ASSERT_EQ(host_ring.allocate_pinned(kCapacity), RingStatus::Ok);
    MockFpgaDmaProducer fpga(host_ring.raw_base());
    ASSERT_TRUE(fpga.is_ready());

    std::thread fpga_thread([&]() {
        std::vector<EventFrame> burst(16);
        std::size_t sent = 0;
        while (sent < kTotalFrames) {
            const std::size_t chunk = std::min<std::size_t>(burst.size(), kTotalFrames - sent);
            for (std::size_t i = 0; i < chunk; ++i) {
                burst[i] = make_dma_frame(sent + i);
            }
            const std::size_t pushed = fpga.dma_write_burst(burst.data(), chunk);
            sent += pushed;
            if (pushed < chunk) {
                // Retry remainder sequentially without counting partial burst mismatch
                for (std::size_t rem = pushed; rem < chunk; ++rem) {
                    while (fpga.dma_write_burst(&burst[rem], 1) == 0) {
                        std::this_thread::yield();
                    }
                    ++sent;
                }
            }
        }
        fpga.signal_device_close();
    });

    std::size_t received = 0;
    std::vector<EventFrame> pop_buf(32);
    while (received < kTotalFrames) {
        const std::size_t n = host_ring.try_pop_batch(std::span<EventFrame>(pop_buf));
        if (n > 0) {
            for (std::size_t i = 0; i < n; ++i) {
                EXPECT_EQ(pop_buf[i].sequence_id, received);
                ++received;
            }
        } else if (host_ring.is_device_closed() && host_ring.empty()) {
            break;
        } else {
            std::this_thread::yield();
        }
    }

    fpga_thread.join();
    EXPECT_EQ(received, kTotalFrames);
}

TEST(PcieCxlRingTest, AttachExternalMmioAndOverflowDropCounter) {
    constexpr std::size_t kCapacity = 16;
    const std::size_t bytes = PcieCxlRing::required_bytes(kCapacity);
    struct alignas(kCacheLineSize) AlignedStorage {
        std::uint8_t buf[sizeof(PcieCxlRingHeader) + kCapacity * sizeof(EventFrame)]{};
    } mmio_window;

    ASSERT_EQ(sizeof(mmio_window.buf), bytes);

    PcieCxlRing host_ring;
    ASSERT_EQ(host_ring.attach_mmio(
                  mmio_window.buf, sizeof(mmio_window.buf), kCapacity,
                  DmaTransportKind::CxlMemDax, 0x10000000ULL, true),
              RingStatus::Ok);
    EXPECT_EQ(host_ring.transport_kind(), DmaTransportKind::CxlMemDax);

    MockFpgaDmaProducer fpga(mmio_window.buf);
    ASSERT_TRUE(fpga.is_ready());

    std::vector<EventFrame> overfill(20);
    for (std::size_t i = 0; i < overfill.size(); ++i) {
        overfill[i] = make_dma_frame(i);
    }
    EXPECT_EQ(fpga.dma_write_burst(std::span<const EventFrame>(overfill)), kCapacity);
    EXPECT_EQ(host_ring.device_dropped_frames(), 4U);

    EventFrame ev{};
    for (std::size_t i = 0; i < kCapacity; ++i) {
        ASSERT_EQ(host_ring.try_pop_status(ev), RingStatus::Ok);
        EXPECT_EQ(ev.sequence_id, i);
    }
    EXPECT_EQ(host_ring.try_pop_status(ev), RingStatus::Empty);
}

} // namespace chronoring::test
