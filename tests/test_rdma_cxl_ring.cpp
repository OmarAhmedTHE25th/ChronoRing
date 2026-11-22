#include "chronoring/event_frame.hpp"
#include "chronoring/rdma_cxl_ring.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace chronoring::test {

namespace {

EventFrame make_fabric_frame(std::uint64_t seq) noexcept {
    EventFrame f{};
    f.timestamp_ns = 9000ULL + seq * 15ULL;
    f.sequence_id = seq;
    f.detector_id = static_cast<std::uint32_t>(seq % 4U);
    f.energy_mev = 40.0f + static_cast<float>(seq % 10U);
    f.quality_flags = kFlagValid | kFlagCalibrated;
    f.multiplicity = 2;
    return f;
}

} // namespace

TEST(RdmaCxlRingTest, OneSidedRdmaWrapAroundAndInvalidRkeyRejection) {
    constexpr std::size_t kCapacity = 64;
    struct alignas(kCacheLineSize) RemoteWindow {
        std::uint8_t bytes[sizeof(PcieCxlRingHeader) + kCapacity * sizeof(EventFrame)]{};
    } remote_mem;

    FabricRingEndpoint consumer;
    ASSERT_EQ(consumer.attach_rdma_consumer(
                  remote_mem.bytes, sizeof(remote_mem.bytes), kCapacity,
                  FabricTransportKind::RdmaInfiniBand),
              RingStatus::Ok);
    ASSERT_TRUE(consumer.is_open());
    EXPECT_EQ(consumer.transport_kind(), FabricTransportKind::RdmaInfiniBand);

    LoopbackRdmaTransport nic;
    const RdmaRegionDescriptor desc =
        nic.register_memory(remote_mem.bytes, sizeof(remote_mem.bytes), kCapacity, 0x7788u);

    // Invalid rkey must fail cleanly
    RdmaRegionDescriptor bad_desc = desc;
    bad_desc.rkey = 0xBADu;
    FabricRingEndpoint bad_prod;
    EXPECT_EQ(bad_prod.open_rdma_producer(&nic, bad_desc, FabricTransportKind::RdmaRoceV2),
              RingStatus::MappingError);

    FabricRingEndpoint producer;
    ASSERT_EQ(producer.open_rdma_producer(&nic, desc, FabricTransportKind::RdmaRoceV2),
              RingStatus::Ok);
    ASSERT_TRUE(producer.is_open());

    // Push 10 batches of 40 frames (wrap around 64-slot ring multiple times)
    std::uint64_t next_seq = 0;
    for (int round = 0; round < 10; ++round) {
        std::vector<EventFrame> batch(40);
        for (std::size_t i = 0; i < batch.size(); ++i) {
            batch[i] = make_fabric_frame(next_seq++);
        }
        EXPECT_EQ(producer.try_push_batch(std::span<const EventFrame>(batch)), 40U);
        EXPECT_EQ(consumer.size_approx(), 40U);

        std::vector<EventFrame> out(40);
        EXPECT_EQ(consumer.try_pop_batch(std::span<EventFrame>(out)), 40U);
        for (std::size_t i = 0; i < out.size(); ++i) {
            EXPECT_EQ(out[i].sequence_id, batch[i].sequence_id);
            EXPECT_FLOAT_EQ(out[i].energy_mev, batch[i].energy_mev);
        }
    }

    nic.unregister_memory(0x7788u);
}

TEST(RdmaCxlRingTest, CxlCoherentAndSharedMemoryFabricModes) {
    // 1. CXL.mem coherent mode
    FabricRingEndpoint cxl_ep;
    ASSERT_EQ(cxl_ep.open_cxl_pinned(64, 0xCAFE0000ULL), RingStatus::Ok);
    EXPECT_EQ(cxl_ep.transport_kind(), FabricTransportKind::CxlMemCoherent);
    std::vector<EventFrame> batch(8);
    for (std::size_t i = 0; i < batch.size(); ++i) {
        batch[i] = make_fabric_frame(i);
    }
    EXPECT_EQ(cxl_ep.try_push_batch(batch), 8U);
    std::vector<EventFrame> out(8);
    EXPECT_EQ(cxl_ep.try_pop_batch(out), 8U);
    EXPECT_EQ(out[7].sequence_id, 7U);

    // 2. Shared-memory fabric mode
    const auto now = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const std::string shm_name = "cr_fab_" + std::to_string(now % 1000000ULL);
    ShmScopeGuard guard(shm_name);

    FabricRingEndpoint shm_prod;
    ASSERT_EQ(shm_prod.open_shm(shm_name, ShmMode::Create, 64, false), RingStatus::Ok);
    FabricRingEndpoint shm_cons;
    ASSERT_EQ(shm_cons.open_shm(shm_name, ShmMode::Open), RingStatus::Ok);

    EXPECT_EQ(shm_prod.try_push_batch(batch), 8U);
    std::vector<EventFrame> shm_out(8);
    EXPECT_EQ(shm_cons.try_pop_batch(shm_out), 8U);
    EXPECT_EQ(shm_out[5].sequence_id, 5U);
}

} // namespace chronoring::test
