#pragma once

#include "chronoring/status.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace chronoring {

struct EncodedStripe {
    std::size_t k_data{0};
    std::size_t m_parity{0};
    std::size_t original_size{0};
    std::size_t shard_size{0};
    std::vector<std::vector<std::uint8_t>> shards{};
    std::vector<std::uint32_t> shard_crc32c{};
};

// Systematic GF(2^8) Cauchy Reed-Solomon erasure coder.
// Encodes arbitrary payloads into K data shards + M parity shards with per-shard CRC32C,
// surviving any M arbitrary shard losses or corruptions when K + M <= 255.
class ErasureCoder {
public:
    ErasureCoder(std::size_t k_data, std::size_t m_parity) noexcept
        : k_data_(k_data), m_parity_(m_parity) {}

    [[nodiscard]] bool is_valid() const noexcept {
        return k_data_ >= 1 && m_parity_ >= 1 && (k_data_ + m_parity_) <= 255U;
    }

    [[nodiscard]] std::size_t k_data() const noexcept { return k_data_; }
    [[nodiscard]] std::size_t m_parity() const noexcept { return m_parity_; }

    [[nodiscard]] Expected<EncodedStripe, RingStatus> encode(
        std::span<const std::uint8_t> data) const;

    [[nodiscard]] Expected<std::vector<std::uint8_t>, RingStatus> reconstruct(
        const EncodedStripe& stripe,
        std::span<const std::uint8_t> present) const;

private:
    std::size_t k_data_{0};
    std::size_t m_parity_{0};
};

} // namespace chronoring
