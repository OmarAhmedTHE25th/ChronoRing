#include "chronoring/erasure_coding.hpp"
#include "chronoring/columnar_sink.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace chronoring {

namespace {

struct GfTables {
    std::array<std::uint8_t, 512> exp{};
    std::array<std::uint8_t, 256> log{};
};

constexpr GfTables build_gf_tables() noexcept {
    GfTables t{};
    std::uint16_t val = 1;
    for (std::size_t i = 0; i < 255; ++i) {
        t.exp[i] = static_cast<std::uint8_t>(val);
        t.log[val] = static_cast<std::uint8_t>(i);
        val = static_cast<std::uint16_t>(val << 1U);
        if ((val & 0x100U) != 0U) {
            val ^= 0x11DU;
        }
    }
    for (std::size_t i = 255; i < 512; ++i) {
        t.exp[i] = t.exp[i - 255];
    }
    return t;
}

inline constexpr GfTables kGf = build_gf_tables();

[[nodiscard]] constexpr std::uint8_t gf_mul(std::uint8_t a, std::uint8_t b) noexcept {
    if (a == 0 || b == 0) {
        return 0;
    }
    return kGf.exp[static_cast<std::size_t>(kGf.log[a]) + static_cast<std::size_t>(kGf.log[b])];
}

[[nodiscard]] constexpr std::uint8_t gf_inv(std::uint8_t a) noexcept {
    if (a == 0) {
        return 0;
    }
    return kGf.exp[255U - static_cast<std::size_t>(kGf.log[a])];
}

[[nodiscard]] std::uint8_t cauchy_coeff(std::size_t row,
                                        std::size_t col,
                                        std::size_t k_data) noexcept {
    if (row < k_data) {
        return (row == col) ? 1U : 0U;
    }
    const std::size_t x = row; // x in [k_data, k_data + m_parity - 1]
    const std::size_t y = col; // y in [0, k_data - 1]
    return gf_inv(static_cast<std::uint8_t>(x ^ y));
}

bool invert_gf_matrix(std::vector<std::uint8_t>& mat, std::size_t n) {
    std::vector<std::uint8_t> aug(n * n * 2U, 0);
    for (std::size_t r = 0; r < n; ++r) {
        for (std::size_t c = 0; c < n; ++c) {
            aug[r * (2U * n) + c] = mat[r * n + c];
        }
        aug[r * (2U * n) + n + r] = 1U;
    }

    for (std::size_t col = 0; col < n; ++col) {
        std::size_t pivot = col;
        while (pivot < n && aug[pivot * (2U * n) + col] == 0) {
            ++pivot;
        }
        if (pivot == n) {
            return false;
        }
        if (pivot != col) {
            for (std::size_t j = 0; j < 2U * n; ++j) {
                std::swap(aug[col * (2U * n) + j], aug[pivot * (2U * n) + j]);
            }
        }
        const std::uint8_t inv_p = gf_inv(aug[col * (2U * n) + col]);
        for (std::size_t j = 0; j < 2U * n; ++j) {
            aug[col * (2U * n) + j] = gf_mul(aug[col * (2U * n) + j], inv_p);
        }
        for (std::size_t r = 0; r < n; ++r) {
            if (r == col) {
                continue;
            }
            const std::uint8_t factor = aug[r * (2U * n) + col];
            if (factor == 0) {
                continue;
            }
            for (std::size_t j = 0; j < 2U * n; ++j) {
                aug[r * (2U * n) + j] ^= gf_mul(factor, aug[col * (2U * n) + j]);
            }
        }
    }

    for (std::size_t r = 0; r < n; ++r) {
        for (std::size_t c = 0; c < n; ++c) {
            mat[r * n + c] = aug[r * (2U * n) + n + c];
        }
    }
    return true;
}

} // namespace

Expected<EncodedStripe, RingStatus> ErasureCoder::encode(
    std::span<const std::uint8_t> data) const {
    if (!is_valid()) {
        return unexpected(RingStatus::InvalidArgument);
    }

    const std::size_t shard_size =
        std::max<std::size_t>(1U, (data.size() + k_data_ - 1U) / k_data_);
    const std::size_t total_shards = k_data_ + m_parity_;

    EncodedStripe stripe;
    stripe.k_data = k_data_;
    stripe.m_parity = m_parity_;
    stripe.original_size = data.size();
    stripe.shard_size = shard_size;
    stripe.shards.assign(total_shards, std::vector<std::uint8_t>(shard_size, 0));
    stripe.shard_crc32c.resize(total_shards, 0);

    // Fill systematic data shards [0 .. k_data - 1]
    for (std::size_t j = 0; j < k_data_; ++j) {
        const std::size_t src_off = j * shard_size;
        if (src_off < data.size()) {
            const std::size_t copy_n = std::min(shard_size, data.size() - src_off);
            std::memcpy(stripe.shards[j].data(), data.data() + src_off, copy_n);
        }
    }

    // Compute parity shards [k_data .. k_data + m_parity - 1]
    for (std::size_t p = 0; p < m_parity_; ++p) {
        const std::size_t row = k_data_ + p;
        auto& parity_buf = stripe.shards[row];
        for (std::size_t j = 0; j < k_data_; ++j) {
            const std::uint8_t coeff = cauchy_coeff(row, j, k_data_);
            const auto& src_buf = stripe.shards[j];
            for (std::size_t b = 0; b < shard_size; ++b) {
                parity_buf[b] ^= gf_mul(coeff, src_buf[b]);
            }
        }
    }

    for (std::size_t s = 0; s < total_shards; ++s) {
        stripe.shard_crc32c[s] = crc32c(stripe.shards[s].data(), shard_size);
    }
    return stripe;
}

Expected<std::vector<std::uint8_t>, RingStatus> ErasureCoder::reconstruct(
    const EncodedStripe& stripe,
    std::span<const std::uint8_t> present) const {
    const std::size_t total_shards = k_data_ + m_parity_;
    if (!is_valid() || stripe.k_data != k_data_ || stripe.m_parity != m_parity_ ||
        stripe.shards.size() != total_shards || stripe.shard_crc32c.size() != total_shards ||
        present.size() != total_shards) {
        return unexpected(RingStatus::InvalidArgument);
    }

    // Filter out missing OR CRC32C-corrupted shards.
    std::vector<std::size_t> valid_indices;
    valid_indices.reserve(k_data_);
    std::vector<std::uint8_t> data_intact(k_data_, 0);

    for (std::size_t i = 0; i < total_shards; ++i) {
        if (present[i] == 0 || stripe.shards[i].size() != stripe.shard_size) {
            continue;
        }
        if (crc32c(stripe.shards[i].data(), stripe.shard_size) != stripe.shard_crc32c[i]) {
            continue;
        }
        if (i < k_data_) {
            data_intact[i] = 1;
        }
        valid_indices.push_back(i);
    }

    // Fast path: all K systematic data shards are intact.
    if (std::all_of(data_intact.begin(), data_intact.end(),
                    [](std::uint8_t ok) { return ok != 0; })) {
        std::vector<std::uint8_t> out(stripe.original_size);
        for (std::size_t j = 0; j < k_data_; ++j) {
            const std::size_t dst_off = j * stripe.shard_size;
            if (dst_off < stripe.original_size) {
                const std::size_t copy_n =
                    std::min(stripe.shard_size, stripe.original_size - dst_off);
                std::memcpy(out.data() + dst_off, stripe.shards[j].data(), copy_n);
            }
        }
        return out;
    }

    if (valid_indices.size() < k_data_) {
        return unexpected(RingStatus::CorruptedPage);
    }

    valid_indices.resize(k_data_);

    // Build K x K submatrix from surviving generator rows and invert over GF(2^8).
    std::vector<std::uint8_t> decode_mat(k_data_ * k_data_);
    for (std::size_t r = 0; r < k_data_; ++r) {
        const std::size_t row = valid_indices[r];
        for (std::size_t c = 0; c < k_data_; ++c) {
            decode_mat[r * k_data_ + c] = cauchy_coeff(row, c, k_data_);
        }
    }
    if (!invert_gf_matrix(decode_mat, k_data_)) {
        return unexpected(RingStatus::CorruptedPage);
    }

    std::vector<std::uint8_t> recovered_full(k_data_ * stripe.shard_size, 0);
    for (std::size_t out_j = 0; out_j < k_data_; ++out_j) {
        std::uint8_t* dst = recovered_full.data() + out_j * stripe.shard_size;
        for (std::size_t r = 0; r < k_data_; ++r) {
            const std::uint8_t w = decode_mat[out_j * k_data_ + r];
            if (w == 0) {
                continue;
            }
            const std::uint8_t* src = stripe.shards[valid_indices[r]].data();
            for (std::size_t b = 0; b < stripe.shard_size; ++b) {
                dst[b] ^= gf_mul(w, src[b]);
            }
        }
    }

    recovered_full.resize(stripe.original_size);
    return recovered_full;
}

} // namespace chronoring
