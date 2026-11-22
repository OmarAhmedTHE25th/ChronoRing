#include "chronoring/crypto_integrity.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>

namespace chronoring {

namespace {

constexpr std::array<std::uint32_t, 64> kSha256K = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

struct Sha256Ctx {
    std::array<std::uint32_t, 8> state{
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    };
    std::uint64_t bitlen{0};
    std::array<std::uint8_t, 64> block{};
    std::size_t block_len{0};

    void compress(const std::uint8_t* blk) noexcept {
        std::array<std::uint32_t, 64> w{};
        for (std::size_t i = 0; i < 16; ++i) {
            const std::size_t j = i * 4U;
            w[i] = (static_cast<std::uint32_t>(blk[j]) << 24U) |
                   (static_cast<std::uint32_t>(blk[j + 1U]) << 16U) |
                   (static_cast<std::uint32_t>(blk[j + 2U]) << 8U) |
                   static_cast<std::uint32_t>(blk[j + 3U]);
        }
        for (std::size_t i = 16; i < 64; ++i) {
            const std::uint32_t s0 =
                std::rotr(w[i - 15], 7) ^ std::rotr(w[i - 15], 18) ^ (w[i - 15] >> 3U);
            const std::uint32_t s1 =
                std::rotr(w[i - 2], 17) ^ std::rotr(w[i - 2], 19) ^ (w[i - 2] >> 10U);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        std::uint32_t a = state[0];
        std::uint32_t b = state[1];
        std::uint32_t c = state[2];
        std::uint32_t d = state[3];
        std::uint32_t e = state[4];
        std::uint32_t f = state[5];
        std::uint32_t g = state[6];
        std::uint32_t h = state[7];

        for (std::size_t i = 0; i < 64; ++i) {
            const std::uint32_t S1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
            const std::uint32_t ch = (e & f) ^ ((~e) & g);
            const std::uint32_t temp1 = h + S1 + ch + kSha256K[i] + w[i];
            const std::uint32_t S0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = S0 + maj;

            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }

        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
        state[5] += f;
        state[6] += g;
        state[7] += h;
    }

    void update(const void* data, std::size_t len) noexcept {
        if (data == nullptr || len == 0) {
            return;
        }
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        bitlen += static_cast<std::uint64_t>(len) * 8ULL;

        std::size_t offset = 0;
        if (block_len > 0) {
            const std::size_t take = std::min(64U - block_len, len);
            std::memcpy(block.data() + block_len, bytes, take);
            block_len += take;
            offset += take;
            if (block_len == 64U) {
                compress(block.data());
                block_len = 0;
            }
        }
        while (offset + 64U <= len) {
            compress(bytes + offset);
            offset += 64U;
        }
        if (offset < len) {
            const std::size_t rem = len - offset;
            std::memcpy(block.data(), bytes + offset, rem);
            block_len = rem;
        }
    }

    [[nodiscard]] Sha256Digest finish() noexcept {
        block[block_len++] = 0x80U;
        if (block_len > 56U) {
            std::memset(block.data() + block_len, 0, 64U - block_len);
            compress(block.data());
            block_len = 0;
        }
        std::memset(block.data() + block_len, 0, 56U - block_len);
        for (int i = 0; i < 8; ++i) {
            block[56U + static_cast<std::size_t>(i)] =
                static_cast<std::uint8_t>((bitlen >> ((7 - i) * 8)) & 0xFFU);
        }
        compress(block.data());

        Sha256Digest out{};
        for (std::size_t i = 0; i < 8; ++i) {
            out[i * 4U] = static_cast<std::uint8_t>((state[i] >> 24U) & 0xFFU);
            out[i * 4U + 1U] = static_cast<std::uint8_t>((state[i] >> 16U) & 0xFFU);
            out[i * 4U + 2U] = static_cast<std::uint8_t>((state[i] >> 8U) & 0xFFU);
            out[i * 4U + 3U] = static_cast<std::uint8_t>(state[i] & 0xFFU);
        }
        return out;
    }
};

Sha256Digest wots_chain_step(const Sha256Digest& cur,
                             std::size_t chain_idx,
                             std::size_t start_step,
                             std::size_t steps) noexcept {
    Sha256Digest val = cur;
    for (std::size_t s = 0; s < steps; ++s) {
        std::array<std::uint8_t, 1 + 2 + 32> buf{};
        buf[0] = 0x02U;
        buf[1] = static_cast<std::uint8_t>(chain_idx & 0xFFU);
        buf[2] = static_cast<std::uint8_t>((start_step + s) & 0xFFU);
        std::memcpy(buf.data() + 3, val.data(), val.size());
        val = sha256(buf.data(), buf.size());
    }
    return val;
}

std::array<std::uint8_t, kWotsChains> wots_base16_digits(const Sha256Digest& msg) noexcept {
    std::array<std::uint8_t, kWotsChains> digits{};
    std::uint32_t checksum = 0;
    for (std::size_t i = 0; i < 32; ++i) {
        const std::uint8_t hi = static_cast<std::uint8_t>((msg[i] >> 4U) & 0x0FU);
        const std::uint8_t lo = static_cast<std::uint8_t>(msg[i] & 0x0FU);
        digits[i * 2U] = hi;
        digits[i * 2U + 1U] = lo;
        checksum += (15U - hi) + (15U - lo);
    }
    digits[64] = static_cast<std::uint8_t>((checksum >> 8U) & 0x0FU);
    digits[65] = static_cast<std::uint8_t>((checksum >> 4U) & 0x0FU);
    digits[66] = static_cast<std::uint8_t>(checksum & 0x0FU);
    return digits;
}

} // namespace

Sha256Digest sha256(const void* data, std::size_t len) noexcept {
    Sha256Ctx ctx;
    ctx.update(data, len);
    return ctx.finish();
}

std::string digest_to_hex(const Sha256Digest& digest) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (std::uint8_t b : digest) {
        out.push_back(kHex[(b >> 4U) & 0x0FU]);
        out.push_back(kHex[b & 0x0FU]);
    }
    return out;
}

bool constant_time_equal(const Sha256Digest& a, const Sha256Digest& b) noexcept {
    std::uint8_t diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<std::uint8_t>(a[i] ^ b[i]);
    }
    return diff == 0;
}

// ============================================================================
// PageMerkleTree
// ============================================================================

Sha256Digest PageMerkleTree::hash_leaf(const void* data, std::size_t len) noexcept {
    Sha256Ctx ctx;
    const std::uint8_t domain = 0x00U;
    ctx.update(&domain, 1);
    ctx.update(data, len);
    return ctx.finish();
}

Sha256Digest PageMerkleTree::hash_node(const Sha256Digest& left,
                                       const Sha256Digest& right) noexcept {
    Sha256Ctx ctx;
    const std::uint8_t domain = 0x01U;
    ctx.update(&domain, 1);
    ctx.update(left.data(), left.size());
    ctx.update(right.data(), right.size());
    return ctx.finish();
}

Sha256Digest PageMerkleTree::append_page(const void* data, std::size_t len) {
    const Sha256Digest leaf = hash_leaf(data, len);
    leaves_.push_back(leaf);
    return leaf;
}

Sha256Digest PageMerkleTree::append_leaf_digest(const Sha256Digest& page_raw_digest) {
    const Sha256Digest leaf = hash_leaf(page_raw_digest.data(), page_raw_digest.size());
    leaves_.push_back(leaf);
    return leaf;
}

Sha256Digest PageMerkleTree::root() const {
    if (leaves_.empty()) {
        return sha256(nullptr, 0);
    }
    std::vector<Sha256Digest> level = leaves_;
    while (level.size() > 1) {
        std::vector<Sha256Digest> next;
        next.reserve((level.size() + 1) / 2);
        for (std::size_t i = 0; i < level.size(); i += 2) {
            if (i + 1 < level.size()) {
                next.push_back(hash_node(level[i], level[i + 1]));
            } else {
                // Promote odd trailing node directly via self-hash pair to maintain uniform height.
                next.push_back(hash_node(level[i], level[i]));
            }
        }
        level = std::move(next);
    }
    return level.front();
}

Expected<MerkleProof, RingStatus> PageMerkleTree::generate_proof(std::size_t leaf_idx) const {
    if (leaf_idx >= leaves_.size()) {
        return unexpected(RingStatus::InvalidArgument);
    }
    MerkleProof proof;
    std::vector<Sha256Digest> level = leaves_;
    std::size_t idx = leaf_idx;

    while (level.size() > 1) {
        const bool is_left = (idx % 2U) == 0U;
        const std::size_t sibling_idx = is_left
                                            ? std::min(idx + 1, level.size() - 1)
                                            : (idx - 1);
        proof.siblings.push_back(level[sibling_idx]);
        proof.sibling_is_right.push_back(is_left);

        std::vector<Sha256Digest> next;
        next.reserve((level.size() + 1) / 2);
        for (std::size_t i = 0; i < level.size(); i += 2) {
            if (i + 1 < level.size()) {
                next.push_back(hash_node(level[i], level[i + 1]));
            } else {
                next.push_back(hash_node(level[i], level[i]));
            }
        }
        level = std::move(next);
        idx /= 2U;
    }
    return proof;
}

bool PageMerkleTree::verify_proof(const Sha256Digest& leaf_hash,
                                  const MerkleProof& proof,
                                  const Sha256Digest& expected_root) noexcept {
    if (proof.siblings.size() != proof.sibling_is_right.size()) {
        return false;
    }
    Sha256Digest cur = leaf_hash;
    for (std::size_t i = 0; i < proof.siblings.size(); ++i) {
        if (proof.sibling_is_right[i]) {
            cur = hash_node(cur, proof.siblings[i]);
        } else {
            cur = hash_node(proof.siblings[i], cur);
        }
    }
    return constant_time_equal(cur, expected_root);
}

// ============================================================================
// Post-Quantum Winternitz OTS (w=16, SHA-256)
// ============================================================================

PqKeyPair pq_generate_keypair(std::uint64_t seed) noexcept {
    PqKeyPair kp{};
    std::array<std::uint8_t, 13> sk_seed{};
    for (std::size_t b = 0; b < 8; ++b) {
        sk_seed[b] = static_cast<std::uint8_t>((seed >> (b * 8)) & 0xFFU);
    }
    sk_seed[12] = 0xA5U;

    for (std::size_t i = 0; i < kWotsChains; ++i) {
        sk_seed[8] = static_cast<std::uint8_t>(i & 0xFFU);
        sk_seed[9] = static_cast<std::uint8_t>((i >> 8U) & 0xFFU);
        sk_seed[10] = 0;
        sk_seed[11] = 0;
        kp.priv.sk[i] = sha256(sk_seed.data(), sk_seed.size());
        kp.pub.pk[i] = wots_chain_step(kp.priv.sk[i], i, 0, 15);
    }
    return kp;
}

PqSignature pq_sign(const Sha256Digest& msg, const PqPrivateKey& sk) noexcept {
    const auto digits = wots_base16_digits(msg);
    PqSignature sig{};
    for (std::size_t i = 0; i < kWotsChains; ++i) {
        sig.sig[i] = wots_chain_step(sk.sk[i], i, 0, digits[i]);
    }
    return sig;
}

bool pq_verify(const Sha256Digest& msg,
               const PqSignature& sig,
               const PqPublicKey& pk) noexcept {
    const auto digits = wots_base16_digits(msg);
    for (std::size_t i = 0; i < kWotsChains; ++i) {
        const std::size_t rem = 15U - digits[i];
        const Sha256Digest derived_pk = wots_chain_step(sig.sig[i], i, digits[i], rem);
        if (!constant_time_equal(derived_pk, pk.pk[i])) {
            return false;
        }
    }
    return true;
}

} // namespace chronoring
