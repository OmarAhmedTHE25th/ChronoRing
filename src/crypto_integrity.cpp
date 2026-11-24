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
        if (block_len > 0 && block_len < block.size()) {
            const std::size_t space = block.size() - block_len;
            const std::size_t take = std::min(space, len);
            std::memcpy(block.data() + block_len, bytes, take);
            block_len += take;
            offset += take;
            if (block_len == block.size()) {
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

// RFC 8391-style WOTS+ chaining function with per-step public-seed keyed hash
// and bitmask XOR:
//   K  = SHA256(0x03 || pub_seed || uint16_be(chain_idx) || uint8(step))
//   BM = SHA256(0x04 || pub_seed || uint16_be(chain_idx) || uint8(step))
//   val' = SHA256(0x02 || K || (val XOR BM))
Sha256Digest wots_plus_chain_step(const Sha256Digest& cur,
                                  const PqSeed256& pub_seed,
                                  std::size_t chain_idx,
                                  std::size_t start_step,
                                  std::size_t steps) noexcept {
    Sha256Digest val = cur;
    for (std::size_t s = 0; s < steps; ++s) {
        const std::uint8_t step = static_cast<std::uint8_t>((start_step + s) & 0xFFU);

        std::array<std::uint8_t, 1 + 32 + 2 + 1> adrs{};
        adrs[0] = 0x03U;
        std::memcpy(adrs.data() + 1, pub_seed.data(), pub_seed.size());
        adrs[33] = static_cast<std::uint8_t>((chain_idx >> 8U) & 0xFFU);
        adrs[34] = static_cast<std::uint8_t>(chain_idx & 0xFFU);
        adrs[35] = step;
        const Sha256Digest key = sha256(adrs.data(), adrs.size());

        adrs[0] = 0x04U;
        const Sha256Digest bitmask = sha256(adrs.data(), adrs.size());

        std::array<std::uint8_t, 1 + 32 + 32> buf{};
        buf[0] = 0x02U;
        std::memcpy(buf.data() + 1, key.data(), key.size());
        for (std::size_t b = 0; b < 32; ++b) {
            buf[33 + b] = static_cast<std::uint8_t>(val[b] ^ bitmask[b]);
        }
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

void secure_zero(void* data, std::size_t len) noexcept {
    if (data == nullptr || len == 0) {
        return;
    }
    volatile std::uint8_t* p = static_cast<volatile std::uint8_t*>(data);
    for (std::size_t i = 0; i < len; ++i) {
        p[i] = 0U;
    }
}

Sha256Digest hmac_sha256(std::span<const std::uint8_t> key,
                         std::span<const std::uint8_t> data) noexcept {
    std::array<std::uint8_t, 64> key_block{};
    if (key.size() > key_block.size()) {
        const Sha256Digest hashed_key = sha256(key.data(), key.size());
        std::memcpy(key_block.data(), hashed_key.data(), hashed_key.size());
    } else if (!key.empty()) {
        std::memcpy(key_block.data(), key.data(), key.size());
    }

    std::array<std::uint8_t, 64> ipad{};
    std::array<std::uint8_t, 64> opad{};
    for (std::size_t i = 0; i < 64; ++i) {
        ipad[i] = static_cast<std::uint8_t>(key_block[i] ^ 0x36U);
        opad[i] = static_cast<std::uint8_t>(key_block[i] ^ 0x5CU);
    }

    Sha256Ctx inner;
    inner.update(ipad.data(), ipad.size());
    if (!data.empty()) {
        inner.update(data.data(), data.size());
    }
    const Sha256Digest inner_digest = inner.finish();

    Sha256Ctx outer;
    outer.update(opad.data(), opad.size());
    outer.update(inner_digest.data(), inner_digest.size());
    const Sha256Digest mac = outer.finish();

    secure_zero(key_block.data(), key_block.size());
    secure_zero(ipad.data(), ipad.size());
    secure_zero(opad.data(), opad.size());
    return mac;
}

Sha256Digest hkdf_sha256_extract(std::span<const std::uint8_t> salt,
                                 std::span<const std::uint8_t> ikm) noexcept {
    if (salt.empty()) {
        static constexpr Sha256Digest kZeroSalt{};
        return hmac_sha256(kZeroSalt, ikm);
    }
    return hmac_sha256(salt, ikm);
}

bool hkdf_sha256_expand(const Sha256Digest& prk,
                        std::span<const std::uint8_t> info,
                        std::span<std::uint8_t> okm) noexcept {
    constexpr std::size_t kHashLen = 32;
    if (okm.empty()) {
        return true;
    }
    const std::size_t n = (okm.size() + kHashLen - 1U) / kHashLen;
    if (n > 255U) {
        return false;
    }

    Sha256Digest prev{};
    std::size_t written = 0;

    // RFC 5869 Section 2.3: T(i) = HMAC-Hash(PRK, T(i-1) || info || i)
    // Build input using streaming HMAC (ipad/opad) to avoid heap allocation.
    std::array<std::uint8_t, 64> ipad{};
    std::array<std::uint8_t, 64> opad{};
    for (std::size_t i = 0; i < 64; ++i) {
        const std::uint8_t kb = (i < prk.size()) ? prk[i] : 0U;
        ipad[i] = static_cast<std::uint8_t>(kb ^ 0x36U);
        opad[i] = static_cast<std::uint8_t>(kb ^ 0x5CU);
    }

    for (std::size_t block = 1; block <= n; ++block) {
        Sha256Ctx inner;
        inner.update(ipad.data(), ipad.size());
        if (block > 1) {
            inner.update(prev.data(), prev.size());
        }
        if (!info.empty()) {
            inner.update(info.data(), info.size());
        }
        const std::uint8_t counter = static_cast<std::uint8_t>(block);
        inner.update(&counter, 1);
        const Sha256Digest inner_digest = inner.finish();

        Sha256Ctx outer;
        outer.update(opad.data(), opad.size());
        outer.update(inner_digest.data(), inner_digest.size());
        prev = outer.finish();

        const std::size_t to_copy = std::min(kHashLen, okm.size() - written);
        std::memcpy(okm.data() + written, prev.data(), to_copy);
        written += to_copy;
    }

    secure_zero(prev.data(), prev.size());
    secure_zero(ipad.data(), ipad.size());
    secure_zero(opad.data(), opad.size());
    return true;
}

Sha256Digest hkdf_sha256(std::span<const std::uint8_t> salt,
                         std::span<const std::uint8_t> ikm,
                         std::span<const std::uint8_t> info) noexcept {
    Sha256Digest prk = hkdf_sha256_extract(salt, ikm);
    Sha256Digest out{};
    (void)hkdf_sha256_expand(prk, info, out);
    secure_zero(prk.data(), prk.size());
    return out;
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
    std::array<std::uint8_t, 65> buf{};
    buf[0] = 0x01U;
    for (std::size_t i = 0; i < 32; ++i) {
        buf[1 + i] = left[i];
        buf[33 + i] = right[i];
    }
    return sha256(buf.data(), buf.size());
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
    std::vector<Sha256Digest> next;
    while (level.size() > 1) {
        const std::size_t n = level.size();
        next.clear();
        next.reserve((n >> 1U) + (n & 1U));
        for (std::size_t i = 0; i < n; i += 2) {
            const Sha256Digest& left = level[i];
            const Sha256Digest& right = (i + 1 < n) ? level[i + 1] : left;
            next.push_back(hash_node(left, right));
        }
        level.swap(next);
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
// Post-Quantum Winternitz OTS+ (WOTS+, w=16, SHA-256, 256-bit seeds)
// ============================================================================

PqPrivateKey::~PqPrivateKey() noexcept {
    secure_zero(sk.data(), sizeof(sk));
    secure_zero(pub_seed.data(), sizeof(pub_seed));
    used = true;
    initialized = false;
}

PqPrivateKey::PqPrivateKey(PqPrivateKey&& other) noexcept
    : pub_seed(other.pub_seed),
      sk(other.sk),
      used(other.used),
      initialized(other.initialized) {
    secure_zero(other.sk.data(), sizeof(other.sk));
    secure_zero(other.pub_seed.data(), sizeof(other.pub_seed));
    other.used = true;
    other.initialized = false;
}

PqPrivateKey& PqPrivateKey::operator=(PqPrivateKey&& other) noexcept {
    if (this != &other) {
        secure_zero(sk.data(), sizeof(sk));
        pub_seed = other.pub_seed;
        sk = other.sk;
        used = other.used;
        initialized = other.initialized;
        secure_zero(other.sk.data(), sizeof(other.sk));
        secure_zero(other.pub_seed.data(), sizeof(other.pub_seed));
        other.used = true;
        other.initialized = false;
    }
    return *this;
}

PqKeyPair pq_generate_keypair(const PqSeed256& secret_seed,
                              const PqSeed256& pub_seed) noexcept {
    PqKeyPair kp{};
    kp.pub.pub_seed = pub_seed;
    kp.priv.pub_seed = pub_seed;
    kp.priv.used = false;
    kp.priv.initialized = true;

    Sha256Digest prk = hkdf_sha256_extract(pub_seed, secret_seed);
    std::array<std::uint8_t, 20> info{
        'c', 'h', 'r', 'o', 'n', 'o', 'r', 'i', 'n', 'g', '/',
        'w', 'o', 't', 's', '+', '/', 's', 0,   0};

    for (std::size_t i = 0; i < kWotsChains; ++i) {
        info[18] = static_cast<std::uint8_t>((i >> 8U) & 0xFFU);
        info[19] = static_cast<std::uint8_t>(i & 0xFFU);
        (void)hkdf_sha256_expand(prk, info, kp.priv.sk[i]);
        kp.pub.pk[i] = wots_plus_chain_step(kp.priv.sk[i], pub_seed, i, 0, 15);
    }

    secure_zero(prk.data(), prk.size());
    return kp;
}

PqKeyPair pq_generate_keypair(const PqSeed256& seed_256) noexcept {
    static constexpr std::uint8_t kSalt[] = "chronoring/wots+/keygen/v2";
    static constexpr std::uint8_t kSkInfo[] = "secret-seed";
    static constexpr std::uint8_t kPkInfo[] = "public-seed";
    const std::span<const std::uint8_t> salt(kSalt, sizeof(kSalt) - 1);
    PqSeed256 secret_seed = hkdf_sha256(
        salt, seed_256, std::span<const std::uint8_t>(kSkInfo, sizeof(kSkInfo) - 1));
    const PqSeed256 pub_seed = hkdf_sha256(
        salt, seed_256, std::span<const std::uint8_t>(kPkInfo, sizeof(kPkInfo) - 1));
    PqKeyPair kp = pq_generate_keypair(secret_seed, pub_seed);
    secure_zero(secret_seed.data(), secret_seed.size());
    return kp;
}

Expected<PqSignature, RingStatus> pq_sign(const Sha256Digest& msg,
                                          PqPrivateKey& sk) noexcept {
    if (!sk.initialized || sk.used) {
        return unexpected(RingStatus::InvalidArgument);
    }
    sk.used = true;

    const auto digits = wots_base16_digits(msg);
    PqSignature sig{};
    for (std::size_t i = 0; i < kWotsChains; ++i) {
        sig.sig[i] = wots_plus_chain_step(sk.sk[i], sk.pub_seed, i, 0, digits[i]);
    }

    // Wipe private chain values immediately after one-time use.
    secure_zero(sk.sk.data(), sizeof(sk.sk));
    return sig;
}

bool pq_verify(const Sha256Digest& msg,
               const PqSignature& sig,
               const PqPublicKey& pk) noexcept {
    const auto digits = wots_base16_digits(msg);
    for (std::size_t i = 0; i < kWotsChains; ++i) {
        const std::size_t rem = 15U - digits[i];
        const Sha256Digest derived_pk =
            wots_plus_chain_step(sig.sig[i], pk.pub_seed, i, digits[i], rem);
        if (!constant_time_equal(derived_pk, pk.pk[i])) {
            return false;
        }
    }
    return true;
}

} // namespace chronoring
