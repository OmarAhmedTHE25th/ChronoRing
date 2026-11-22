#include "chronoring/event_crypto.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>

namespace chronoring {

namespace {

[[nodiscard]] inline std::uint32_t load_le32(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8U) |
           (static_cast<std::uint32_t>(p[2]) << 16U) |
           (static_cast<std::uint32_t>(p[3]) << 24U);
}

inline void store_le32(std::uint8_t* p, std::uint32_t v) noexcept {
    p[0] = static_cast<std::uint8_t>(v & 0xFFU);
    p[1] = static_cast<std::uint8_t>((v >> 8U) & 0xFFU);
    p[2] = static_cast<std::uint8_t>((v >> 16U) & 0xFFU);
    p[3] = static_cast<std::uint8_t>((v >> 24U) & 0xFFU);
}

inline void store_le64(std::uint8_t* p, std::uint64_t v) noexcept {
    for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<std::uint8_t>((v >> (i * 8)) & 0xFFULL);
    }
}

inline void quarter_round(std::uint32_t& a,
                          std::uint32_t& b,
                          std::uint32_t& c,
                          std::uint32_t& d) noexcept {
    a += b; d ^= a; d = std::rotl(d, 16);
    c += d; b ^= c; b = std::rotl(b, 12);
    a += b; d ^= a; d = std::rotl(d, 8);
    c += d; b ^= c; b = std::rotl(b, 7);
}

void chacha20_block(const CryptoKey256& key,
                    std::uint32_t counter,
                    const Nonce96& nonce,
                    std::array<std::uint8_t, 64>& out) noexcept {
    std::array<std::uint32_t, 16> st{
        0x61707865u, 0x3320646eu, 0x79622d32u, 0x6b206574u,
        load_le32(key.data() + 0), load_le32(key.data() + 4),
        load_le32(key.data() + 8), load_le32(key.data() + 12),
        load_le32(key.data() + 16), load_le32(key.data() + 20),
        load_le32(key.data() + 24), load_le32(key.data() + 28),
        counter,
        load_le32(nonce.data() + 0), load_le32(nonce.data() + 4),
        load_le32(nonce.data() + 8)
    };
    std::array<std::uint32_t, 16> x = st;
    for (int r = 0; r < 10; ++r) {
        quarter_round(x[0], x[4], x[8], x[12]);
        quarter_round(x[1], x[5], x[9], x[13]);
        quarter_round(x[2], x[6], x[10], x[14]);
        quarter_round(x[3], x[7], x[11], x[15]);
        quarter_round(x[0], x[5], x[10], x[15]);
        quarter_round(x[1], x[6], x[11], x[12]);
        quarter_round(x[2], x[7], x[8], x[13]);
        quarter_round(x[3], x[4], x[9], x[14]);
    }
    for (std::size_t i = 0; i < 16; ++i) {
        store_le32(out.data() + i * 4U, x[i] + st[i]);
    }
}

void chacha20_xor(const CryptoKey256& key,
                  std::uint32_t initial_counter,
                  const Nonce96& nonce,
                  const std::uint8_t* in,
                  std::uint8_t* out,
                  std::size_t len) noexcept {
    std::array<std::uint8_t, 64> keystream{};
    std::uint32_t ctr = initial_counter;
    std::size_t off = 0;
    while (off < len) {
        chacha20_block(key, ctr++, nonce, keystream);
        const std::size_t chunk = std::min<std::size_t>(64U, len - off);
        for (std::size_t i = 0; i < chunk; ++i) {
            out[off + i] = static_cast<std::uint8_t>(in[off + i] ^ keystream[i]);
        }
        off += chunk;
    }
}

class Poly1305 {
public:
    explicit Poly1305(const std::uint8_t otk[32]) noexcept {
        std::array<std::uint8_t, 16> r{};
        std::memcpy(r.data(), otk, 16);
        r[3] &= 15; r[7] &= 15; r[11] &= 15; r[15] &= 15;
        r[4] &= 252; r[8] &= 252; r[12] &= 252;

        const std::uint32_t t0 = load_le32(r.data() + 0);
        const std::uint32_t t1 = load_le32(r.data() + 4);
        const std::uint32_t t2 = load_le32(r.data() + 8);
        const std::uint32_t t3 = load_le32(r.data() + 12);

        r_[0] = t0 & 0x3FFFFFFu;
        r_[1] = ((t0 >> 26) | (t1 << 6)) & 0x3FFFFFFu;
        r_[2] = ((t1 >> 20) | (t2 << 12)) & 0x3FFFFFFu;
        r_[3] = ((t2 >> 14) | (t3 << 18)) & 0x3FFFFFFu;
        r_[4] = (t3 >> 8) & 0x3FFFFFFu;

        pad_[0] = load_le32(otk + 16);
        pad_[1] = load_le32(otk + 20);
        pad_[2] = load_le32(otk + 24);
        pad_[3] = load_le32(otk + 28);
    }

    void update(const std::uint8_t* m, std::size_t bytes) noexcept {
        if (m == nullptr || bytes == 0) {
            return;
        }
        std::size_t off = 0;
        if (leftover_ > 0) {
            const std::size_t want = std::min<std::size_t>(16U - leftover_, bytes);
            std::memcpy(buf_.data() + leftover_, m, want);
            leftover_ += want;
            off += want;
            if (leftover_ == 16U) {
                process_block(buf_.data(), false);
                leftover_ = 0;
            }
        }
        while (off + 16U <= bytes) {
            process_block(m + off, false);
            off += 16U;
        }
        if (off < bytes) {
            leftover_ = bytes - off;
            std::memcpy(buf_.data(), m + off, leftover_);
        }
    }

    void pad16_zeros(std::size_t total_len) noexcept {
        const std::size_t rem = total_len % 16U;
        if (rem != 0) {
            static constexpr std::array<std::uint8_t, 16> kZeros{};
            update(kZeros.data(), 16U - rem);
        }
    }

    [[nodiscard]] AuthTag128 finish() noexcept {
        if (leftover_ > 0) {
            buf_[leftover_] = 1U;
            for (std::size_t i = leftover_ + 1U; i < 16U; ++i) {
                buf_[i] = 0;
            }
            process_block(buf_.data(), true);
        }

        std::uint32_t c = h_[1] >> 26; h_[1] &= 0x3FFFFFFu;
        h_[2] += c; c = h_[2] >> 26; h_[2] &= 0x3FFFFFFu;
        h_[3] += c; c = h_[3] >> 26; h_[3] &= 0x3FFFFFFu;
        h_[4] += c; c = h_[4] >> 26; h_[4] &= 0x3FFFFFFu;
        h_[0] += c * 5u; c = h_[0] >> 26; h_[0] &= 0x3FFFFFFu;
        h_[1] += c;

        std::array<std::uint32_t, 5> g = h_;
        g[0] += 5u; c = g[0] >> 26; g[0] &= 0x3FFFFFFu;
        g[1] += c; c = g[1] >> 26; g[1] &= 0x3FFFFFFu;
        g[2] += c; c = g[2] >> 26; g[2] &= 0x3FFFFFFu;
        g[3] += c; c = g[3] >> 26; g[3] &= 0x3FFFFFFu;
        g[4] += c - (1u << 26);

        const std::uint32_t mask = (g[4] >> 31) - 1u;
        for (std::size_t i = 0; i < 5; ++i) {
            h_[i] = (h_[i] & ~mask) | (g[i] & mask);
        }

        const std::uint64_t f0 =
            static_cast<std::uint64_t>((h_[0]) | (h_[1] << 26)) + pad_[0];
        const std::uint64_t f1 =
            static_cast<std::uint64_t>((h_[1] >> 6) | (h_[2] << 20)) + pad_[1] + (f0 >> 32);
        const std::uint64_t f2 =
            static_cast<std::uint64_t>((h_[2] >> 12) | (h_[3] << 14)) + pad_[2] + (f1 >> 32);
        const std::uint64_t f3 =
            static_cast<std::uint64_t>((h_[3] >> 18) | (h_[4] << 8)) + pad_[3] + (f2 >> 32);

        AuthTag128 tag{};
        store_le32(tag.data() + 0, static_cast<std::uint32_t>(f0));
        store_le32(tag.data() + 4, static_cast<std::uint32_t>(f1));
        store_le32(tag.data() + 8, static_cast<std::uint32_t>(f2));
        store_le32(tag.data() + 12, static_cast<std::uint32_t>(f3));
        return tag;
    }

private:
    void process_block(const std::uint8_t m[16], bool is_final_partial) noexcept {
        const std::uint32_t hibit = is_final_partial ? 0u : (1u << 24);
        const std::uint32_t t0 = load_le32(m + 0);
        const std::uint32_t t1 = load_le32(m + 4);
        const std::uint32_t t2 = load_le32(m + 8);
        const std::uint32_t t3 = load_le32(m + 12);

        h_[0] += t0 & 0x3FFFFFFu;
        h_[1] += ((t0 >> 26) | (t1 << 6)) & 0x3FFFFFFu;
        h_[2] += ((t1 >> 20) | (t2 << 12)) & 0x3FFFFFFu;
        h_[3] += ((t2 >> 14) | (t3 << 18)) & 0x3FFFFFFu;
        h_[4] += (t3 >> 8) | hibit;

        const std::uint64_t s1 = r_[1] * 5ULL;
        const std::uint64_t s2 = r_[2] * 5ULL;
        const std::uint64_t s3 = r_[3] * 5ULL;
        const std::uint64_t s4 = r_[4] * 5ULL;

        std::uint64_t d0 =
            static_cast<std::uint64_t>(h_[0]) * r_[0] +
            static_cast<std::uint64_t>(h_[1]) * s4 +
            static_cast<std::uint64_t>(h_[2]) * s3 +
            static_cast<std::uint64_t>(h_[3]) * s2 +
            static_cast<std::uint64_t>(h_[4]) * s1;
        std::uint64_t d1 =
            static_cast<std::uint64_t>(h_[0]) * r_[1] +
            static_cast<std::uint64_t>(h_[1]) * r_[0] +
            static_cast<std::uint64_t>(h_[2]) * s4 +
            static_cast<std::uint64_t>(h_[3]) * s3 +
            static_cast<std::uint64_t>(h_[4]) * s2;
        std::uint64_t d2 =
            static_cast<std::uint64_t>(h_[0]) * r_[2] +
            static_cast<std::uint64_t>(h_[1]) * r_[1] +
            static_cast<std::uint64_t>(h_[2]) * r_[0] +
            static_cast<std::uint64_t>(h_[3]) * s4 +
            static_cast<std::uint64_t>(h_[4]) * s3;
        std::uint64_t d3 =
            static_cast<std::uint64_t>(h_[0]) * r_[3] +
            static_cast<std::uint64_t>(h_[1]) * r_[2] +
            static_cast<std::uint64_t>(h_[2]) * r_[1] +
            static_cast<std::uint64_t>(h_[3]) * r_[0] +
            static_cast<std::uint64_t>(h_[4]) * s4;
        std::uint64_t d4 =
            static_cast<std::uint64_t>(h_[0]) * r_[4] +
            static_cast<std::uint64_t>(h_[1]) * r_[3] +
            static_cast<std::uint64_t>(h_[2]) * r_[2] +
            static_cast<std::uint64_t>(h_[3]) * r_[1] +
            static_cast<std::uint64_t>(h_[4]) * r_[0];

        std::uint64_t c = d0 >> 26; h_[0] = static_cast<std::uint32_t>(d0 & 0x3FFFFFFULL);
        d1 += c; c = d1 >> 26; h_[1] = static_cast<std::uint32_t>(d1 & 0x3FFFFFFULL);
        d2 += c; c = d2 >> 26; h_[2] = static_cast<std::uint32_t>(d2 & 0x3FFFFFFULL);
        d3 += c; c = d3 >> 26; h_[3] = static_cast<std::uint32_t>(d3 & 0x3FFFFFFULL);
        d4 += c; c = d4 >> 26; h_[4] = static_cast<std::uint32_t>(d4 & 0x3FFFFFFULL);
        h_[0] += static_cast<std::uint32_t>(c * 5ULL);
        c = h_[0] >> 26;
        h_[0] &= 0x3FFFFFFu;
        h_[1] += static_cast<std::uint32_t>(c);
    }

    std::array<std::uint32_t, 5> r_{};
    std::array<std::uint32_t, 5> h_{};
    std::array<std::uint32_t, 4> pad_{};
    std::array<std::uint8_t, 16> buf_{};
    std::size_t leftover_{0};
};

AuthTag128 compute_aead_tag(const CryptoKey256& key,
                            const Nonce96& nonce,
                            std::span<const std::uint8_t> aad,
                            std::span<const std::uint8_t> ciphertext) noexcept {
    std::array<std::uint8_t, 64> otk_block{};
    chacha20_block(key, 0u, nonce, otk_block);
    Poly1305 poly(otk_block.data());
    poly.update(aad.data(), aad.size());
    poly.pad16_zeros(aad.size());
    poly.update(ciphertext.data(), ciphertext.size());
    poly.pad16_zeros(ciphertext.size());
    std::array<std::uint8_t, 16> lens{};
    store_le64(lens.data(), static_cast<std::uint64_t>(aad.size()));
    store_le64(lens.data() + 8, static_cast<std::uint64_t>(ciphertext.size()));
    poly.update(lens.data(), lens.size());
    return poly.finish();
}

} // namespace

void chacha20_poly1305_encrypt(const CryptoKey256& key,
                               const Nonce96& nonce,
                               std::span<const std::uint8_t> aad,
                               std::span<const std::uint8_t> plaintext,
                               std::uint8_t* out_ciphertext,
                               AuthTag128& out_tag) noexcept {
    if (!plaintext.empty() && out_ciphertext != nullptr) {
        chacha20_xor(key, 1u, nonce, plaintext.data(), out_ciphertext, plaintext.size());
    }
    const std::span<const std::uint8_t> ct_span(
        out_ciphertext, (out_ciphertext != nullptr) ? plaintext.size() : 0U);
    out_tag = compute_aead_tag(key, nonce, aad, ct_span);
}

bool chacha20_poly1305_decrypt(const CryptoKey256& key,
                               const Nonce96& nonce,
                               std::span<const std::uint8_t> aad,
                               std::span<const std::uint8_t> ciphertext,
                               const AuthTag128& tag,
                               std::uint8_t* out_plaintext) noexcept {
    const AuthTag128 expected_tag = compute_aead_tag(key, nonce, aad, ciphertext);
    std::uint8_t diff = 0;
    for (std::size_t i = 0; i < tag.size(); ++i) {
        diff |= static_cast<std::uint8_t>(tag[i] ^ expected_tag[i]);
    }
    if (diff != 0) {
        return false;
    }
    if (!ciphertext.empty() && out_plaintext != nullptr) {
        chacha20_xor(key, 1u, nonce, ciphertext.data(), out_plaintext, ciphertext.size());
    }
    return true;
}

// ============================================================================
// DetectorKeyVault & Encrypted Triggering
// ============================================================================

void DetectorKeyVault::set_detector_key(std::uint32_t detector_id,
                                        const CryptoKey256& key) {
    keys_[detector_id] = key;
}

void DetectorKeyVault::derive_detector_key(const CryptoKey256& master_key,
                                           std::uint32_t detector_id) {
    std::array<std::uint8_t, 36> buf{};
    std::memcpy(buf.data(), master_key.data(), master_key.size());
    store_le32(buf.data() + 32, detector_id);
    keys_[detector_id] = sha256(buf.data(), buf.size());
}

Expected<CryptoKey256, RingStatus> DetectorKeyVault::get_key(
    std::uint32_t detector_id) const noexcept {
    const auto it = keys_.find(detector_id);
    if (it == keys_.end()) {
        return unexpected(RingStatus::InvalidArgument);
    }
    return it->second;
}

std::uint64_t ope_encode_f32(float value, const CryptoKey256& key) noexcept {
    if (value == 0.0f) {
        value = 0.0f;
    }
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const std::uint32_t ordered =
        ((bits & 0x80000000u) != 0u) ? ~bits : (bits ^ 0x80000000u);

    const std::uint64_t scale =
        static_cast<std::uint64_t>((load_le32(key.data()) & 0x7FFFFFFEu) + 2u);
    const std::uint64_t bias = static_cast<std::uint64_t>(load_le32(key.data() + 4));
    return static_cast<std::uint64_t>(ordered) * scale + bias;
}

Sha256Digest compute_flag_trapdoor(std::uint32_t quality_flags,
                                   const CryptoKey256& key) noexcept {
    std::array<std::uint8_t, 36> buf{};
    std::memcpy(buf.data(), key.data(), key.size());
    store_le32(buf.data() + 32, quality_flags & kSearchableFlagMask);
    return sha256(buf.data(), buf.size());
}

Expected<EncryptedEventFrame, RingStatus> encrypt_event_frame(
    const EventFrame& frame,
    const DetectorKeyVault& vault) noexcept {
    const auto key_res = vault.get_key(frame.detector_id);
    if (!key_res.has_value()) {
        return unexpected(key_res.error());
    }
    const CryptoKey256& key = *key_res;

    EncryptedEventFrame enc{};
    enc.detector_id = frame.detector_id;
    enc.sequence_id = frame.sequence_id;
    store_le32(enc.nonce.data(), frame.detector_id);
    store_le64(enc.nonce.data() + 4, frame.sequence_id);

    std::array<std::uint8_t, 12> aad{};
    store_le32(aad.data(), frame.detector_id);
    store_le64(aad.data() + 4, frame.sequence_id);

    const auto* plain = reinterpret_cast<const std::uint8_t*>(&frame);
    chacha20_poly1305_encrypt(
        key,
        enc.nonce,
        aad,
        std::span<const std::uint8_t>(plain, sizeof(EventFrame)),
        enc.ciphertext.data(),
        enc.tag);

    enc.ope_energy_token = ope_encode_f32(frame.energy_mev, key);
    enc.ope_abs_timing_token = ope_encode_f32(std::fabs(frame.timing_ps), key);
    enc.ope_pileup_token = ope_encode_f32(frame.pileup_score, key);
    enc.flag_trapdoor = compute_flag_trapdoor(frame.quality_flags, key);
    return enc;
}

Expected<EventFrame, RingStatus> decrypt_event_frame(
    const EncryptedEventFrame& enc,
    const DetectorKeyVault& vault) noexcept {
    const auto key_res = vault.get_key(enc.detector_id);
    if (!key_res.has_value()) {
        return unexpected(key_res.error());
    }
    std::array<std::uint8_t, 12> aad{};
    store_le32(aad.data(), enc.detector_id);
    store_le64(aad.data() + 4, enc.sequence_id);

    EventFrame out{};
    if (!chacha20_poly1305_decrypt(
            *key_res,
            enc.nonce,
            aad,
            enc.ciphertext,
            enc.tag,
            reinterpret_cast<std::uint8_t*>(&out))) {
        return unexpected(RingStatus::CorruptedPage);
    }
    return out;
}

Expected<EncryptedTriggerTrapdoor, RingStatus> make_encrypted_trigger_trapdoor(
    std::uint32_t detector_id,
    const TriggerConfig& cfg,
    const DetectorKeyVault& vault,
    std::uint32_t expected_healthy_flags) noexcept {
    const auto key_res = vault.get_key(detector_id);
    if (!key_res.has_value()) {
        return unexpected(key_res.error());
    }
    const CryptoKey256& key = *key_res;

    EncryptedTriggerTrapdoor td{};
    td.detector_id = detector_id;
    td.check_flags = true;
    td.min_energy_tok = ope_encode_f32(cfg.min_energy_mev, key);
    td.max_energy_tok = ope_encode_f32(cfg.max_energy_mev, key);
    td.max_abs_timing_tok = ope_encode_f32(cfg.max_abs_timing_ps, key);
    td.max_pileup_tok = ope_encode_f32(cfg.max_pileup_score, key);
    td.required_flag_trapdoor = compute_flag_trapdoor(expected_healthy_flags, key);
    return td;
}

bool evaluate_encrypted_trigger(const EncryptedEventFrame& enc,
                                const EncryptedTriggerTrapdoor& trapdoor) noexcept {
    if (enc.detector_id != trapdoor.detector_id) {
        return false;
    }
    if (enc.ope_energy_token < trapdoor.min_energy_tok ||
        enc.ope_energy_token > trapdoor.max_energy_tok) {
        return false;
    }
    if (enc.ope_abs_timing_token > trapdoor.max_abs_timing_tok ||
        enc.ope_pileup_token > trapdoor.max_pileup_tok) {
        return false;
    }
    if (trapdoor.check_flags &&
        !constant_time_equal(enc.flag_trapdoor, trapdoor.required_flag_trapdoor)) {
        return false;
    }
    return true;
}

} // namespace chronoring
