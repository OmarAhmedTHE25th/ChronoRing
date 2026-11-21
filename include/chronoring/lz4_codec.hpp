#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace chronoring {

// Zero-allocation LZ4-format block compressor and decompressor.
// Uses a fixed 16 KiB stack hash table on compression and performs strict bounds
// checking on decompression so corrupted or fuzzed streams fail safely.

[[nodiscard]] constexpr std::size_t lz4_compress_bound(std::size_t input_size) noexcept {
    return input_size + (input_size / 255U) + 16U;
}

namespace detail {

[[nodiscard]] inline std::uint32_t load_u32(const std::uint8_t* p) noexcept {
    std::uint32_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

[[nodiscard]] inline std::uint16_t load_u16_le(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[0]) |
                                      (static_cast<std::uint16_t>(p[1]) << 8U));
}

inline void store_u16_le(std::uint8_t* p, std::uint16_t v) noexcept {
    p[0] = static_cast<std::uint8_t>(v & 0xFFU);
    p[1] = static_cast<std::uint8_t>((v >> 8U) & 0xFFU);
}

[[nodiscard]] inline std::size_t hash_u32(std::uint32_t v) noexcept {
    return static_cast<std::size_t>((v * 2654435761U) >> 20U) & 0xFFFU;
}

inline bool write_extended_length(std::size_t len, std::uint8_t*& op,
                                  const std::uint8_t* oend) noexcept {
    while (len >= 255U) {
        if (op >= oend) {
            return false;
        }
        *op++ = 255U;
        len -= 255U;
    }
    if (op >= oend) {
        return false;
    }
    *op++ = static_cast<std::uint8_t>(len);
    return true;
}

} // namespace detail

[[nodiscard]] inline std::size_t lz4_compress(const std::uint8_t* src,
                                              std::size_t src_len,
                                              std::uint8_t* dst,
                                              std::size_t dst_cap) noexcept {
    if (src == nullptr || dst == nullptr || src_len == 0 || dst_cap == 0) {
        return 0;
    }

    constexpr std::size_t kMinMatch = 4;
    constexpr std::size_t kLastLiterals = 5;
    constexpr std::size_t kMFLimit = 12;
    constexpr std::size_t kMaxDistance = 65535;

    std::uint8_t* op = dst;
    const std::uint8_t* const oend = dst + dst_cap;
    const std::uint8_t* const iend = src + src_len;

    if (src_len < kMFLimit) {
        if (op >= oend) {
            return 0;
        }
        std::uint8_t* const token = op++;
        if (src_len >= 15U) {
            *token = 0xF0U;
            if (!detail::write_extended_length(src_len - 15U, op, oend)) {
                return 0;
            }
        } else {
            *token = static_cast<std::uint8_t>(src_len << 4U);
        }
        if (static_cast<std::size_t>(oend - op) < src_len) {
            return 0;
        }
        std::memcpy(op, src, src_len);
        op += src_len;
        return static_cast<std::size_t>(op - dst);
    }

    std::array<std::uint32_t, 4096> hash_table{};
    hash_table.fill(0);

    const std::uint8_t* const match_limit = iend - kLastLiterals;
    const std::uint8_t* const mf_limit = iend - kMFLimit;
    const std::uint8_t* anchor = src;
    const std::uint8_t* ip = src + 1;

    while (ip <= mf_limit) {
        const std::uint32_t seq = detail::load_u32(ip);
        const std::size_t h = detail::hash_u32(seq);
        const std::uint32_t ref_idx = hash_table[h];
        const std::size_t cur_idx = static_cast<std::size_t>(ip - src);
        hash_table[h] = static_cast<std::uint32_t>(cur_idx);

        const std::uint8_t* ref = src + ref_idx;
        const std::size_t dist = cur_idx - static_cast<std::size_t>(ref_idx);

        if (dist == 0 || dist > kMaxDistance || detail::load_u32(ref) != seq) {
            ++ip;
            continue;
        }

        // Emit literals + match token
        const std::size_t lit_len = static_cast<std::size_t>(ip - anchor);
        if (op >= oend) {
            return 0;
        }
        std::uint8_t* const token = op++;
        if (lit_len >= 15U) {
            *token = 0xF0U;
            if (!detail::write_extended_length(lit_len - 15U, op, oend)) {
                return 0;
            }
        } else {
            *token = static_cast<std::uint8_t>(lit_len << 4U);
        }

        if (static_cast<std::size_t>(oend - op) < lit_len + 2U) {
            return 0;
        }
        if (lit_len > 0) {
            std::memcpy(op, anchor, lit_len);
            op += lit_len;
        }

        detail::store_u16_le(op, static_cast<std::uint16_t>(dist));
        op += 2;

        // Extend match forward
        ip += kMinMatch;
        ref += kMinMatch;
        while (ip < match_limit && *ip == *ref) {
            ++ip;
            ++ref;
        }

        const std::size_t match_len =
            static_cast<std::size_t>(ip - (src + cur_idx)) - kMinMatch;
        if (match_len >= 15U) {
            *token = static_cast<std::uint8_t>(*token | 0x0FU);
            if (!detail::write_extended_length(match_len - 15U, op, oend)) {
                return 0;
            }
        } else {
            *token = static_cast<std::uint8_t>(*token | match_len);
        }

        anchor = ip;
        if (ip <= mf_limit) {
            const std::uint32_t tail_seq = detail::load_u32(ip - 2);
            hash_table[detail::hash_u32(tail_seq)] =
                static_cast<std::uint32_t>((ip - 2) - src);
        }
    }

    // Emit final literals
    const std::size_t last_lits = static_cast<std::size_t>(iend - anchor);
    if (op >= oend) {
        return 0;
    }
    std::uint8_t* const token = op++;
    if (last_lits >= 15U) {
        *token = 0xF0U;
        if (!detail::write_extended_length(last_lits - 15U, op, oend)) {
            return 0;
        }
    } else {
        *token = static_cast<std::uint8_t>(last_lits << 4U);
    }
    if (static_cast<std::size_t>(oend - op) < last_lits) {
        return 0;
    }
    if (last_lits > 0) {
        std::memcpy(op, anchor, last_lits);
        op += last_lits;
    }
    return static_cast<std::size_t>(op - dst);
}

[[nodiscard]] inline std::size_t lz4_decompress(const std::uint8_t* src,
                                                std::size_t src_len,
                                                std::uint8_t* dst,
                                                std::size_t dst_cap) noexcept {
    if (src == nullptr || dst == nullptr || src_len == 0 || dst_cap == 0) {
        return 0;
    }

    const std::uint8_t* ip = src;
    const std::uint8_t* const iend = src + src_len;
    std::uint8_t* op = dst;
    const std::uint8_t* const oend = dst + dst_cap;

    while (ip < iend) {
        const std::uint8_t token = *ip++;
        std::size_t lit_len = static_cast<std::size_t>(token >> 4U);
        if (lit_len == 15U) {
            std::uint8_t s = 0;
            do {
                if (ip >= iend) {
                    return 0;
                }
                s = *ip++;
                lit_len += s;
                if (lit_len > static_cast<std::size_t>(iend - ip)) {
                    return 0;
                }
            } while (s == 255U);
        }

        if (lit_len > static_cast<std::size_t>(iend - ip) ||
            lit_len > static_cast<std::size_t>(oend - op)) {
            return 0;
        }
        if (lit_len > 0) {
            std::memcpy(op, ip, lit_len);
            ip += lit_len;
            op += lit_len;
        }

        if (ip == iend) {
            break;
        }
        if (static_cast<std::size_t>(iend - ip) < 2U) {
            return 0;
        }
        const std::size_t offset = detail::load_u16_le(ip);
        ip += 2;
        if (offset == 0 || offset > static_cast<std::size_t>(op - dst)) {
            return 0;
        }

        std::size_t match_len = static_cast<std::size_t>(token & 0x0FU) + 4U;
        if ((token & 0x0FU) == 15U) {
            std::uint8_t s = 0;
            do {
                if (ip >= iend) {
                    return 0;
                }
                s = *ip++;
                match_len += s;
                if (match_len > static_cast<std::size_t>(oend - op)) {
                    return 0;
                }
            } while (s == 255U);
        }

        if (match_len > static_cast<std::size_t>(oend - op)) {
            return 0;
        }

        const std::uint8_t* match = op - offset;
        for (std::size_t i = 0; i < match_len; ++i) {
            op[i] = match[i];
        }
        op += match_len;
    }

    return static_cast<std::size_t>(op - dst);
}

} // namespace chronoring
