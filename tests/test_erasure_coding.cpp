#include "chronoring/erasure_coding.hpp"
#include "chronoring/event_frame.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <span>
#include <vector>

namespace chronoring::test {

TEST(ErasureCodingTest, FourPlusTwoSurvivesAnyTwoShardLosses) {
    ErasureCoder coder(4, 2);
    ASSERT_TRUE(coder.is_valid());

    std::vector<std::uint8_t> payload(sizeof(ColumnarPage));
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<std::uint8_t>((i * 37U + (i >> 8U)) & 0xFFU);
    }

    const auto encoded = coder.encode(std::span<const std::uint8_t>(payload));
    ASSERT_TRUE(encoded.has_value());
    EXPECT_EQ(encoded->shards.size(), 6U);

    // 1. All present
    std::vector<std::uint8_t> present(6, 1);
    auto dec0 = coder.reconstruct(*encoded, present);
    ASSERT_TRUE(dec0.has_value());
    EXPECT_EQ(*dec0, payload);

    // 2. Lose EVERY possible pair of shards (15 combinations) -> bit-exact reconstruction
    for (std::size_t a = 0; a < 6; ++a) {
        for (std::size_t b = a + 1; b < 6; ++b) {
            std::vector<std::uint8_t> mask(6, 1);
            mask[a] = 0;
            mask[b] = 0;
            EncodedStripe erased = *encoded;
            erased.shards[a].assign(erased.shard_size, 0xFFU);
            erased.shards[b].assign(erased.shard_size, 0x00U);

            const auto rec = coder.reconstruct(erased, mask);
            ASSERT_TRUE(rec.has_value()) << "Failed on missing (" << a << ", " << b << ")";
            EXPECT_EQ(*rec, payload) << "Mismatch on missing (" << a << ", " << b << ")";
        }
    }

    // 3. Lose 3 shards (> M=2) -> must report CorruptedPage
    std::vector<std::uint8_t> three_lost(6, 1);
    three_lost[0] = 0;
    three_lost[2] = 0;
    three_lost[4] = 0;
    const auto fail_res = coder.reconstruct(*encoded, three_lost);
    ASSERT_FALSE(fail_res.has_value());
    EXPECT_EQ(fail_res.error(), RingStatus::CorruptedPage);
}

TEST(ErasureCodingTest, SixPlusThreeAndSilentCorruptionDetection) {
    ErasureCoder coder(6, 3);
    ASSERT_TRUE(coder.is_valid());

    // Non-multiple size to test zero-padding / exact original_size trim
    std::vector<std::uint8_t> payload(13777);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<std::uint8_t>((i ^ 0x5AU) & 0xFFU);
    }

    const auto encoded = coder.encode(std::span<const std::uint8_t>(payload));
    ASSERT_TRUE(encoded.has_value());

    // Silently corrupt 2 data shards without clearing their present flag + mark 1 parity absent
    EncodedStripe corrupted = *encoded;
    corrupted.shards[1][12] ^= 0x7FU;
    corrupted.shards[4][99] ^= 0x01U;
    std::vector<std::uint8_t> present(9, 1);
    present[7] = 0; // total faults = 3 <= M=3

    const auto rec = coder.reconstruct(corrupted, present);
    ASSERT_TRUE(rec.has_value());
    EXPECT_EQ(*rec, payload);
}

} // namespace chronoring::test
