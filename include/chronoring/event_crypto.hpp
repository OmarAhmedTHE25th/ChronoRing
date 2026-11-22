#pragma once

#include "chronoring/crypto_integrity.hpp"
#include "chronoring/event_frame.hpp"
#include "chronoring/simd_trigger.hpp"
#include "chronoring/status.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>

namespace chronoring {

using CryptoKey256 = std::array<std::uint8_t, 32>;
using Nonce96 = std::array<std::uint8_t, 12>;
using AuthTag128 = std::array<std::uint8_t, 16>;

inline constexpr std::uint32_t kSearchableFlagMask =
    kFlagValid | kFlagCalibrated | kFlagSaturated | kFlagVeto;

// Pure C++20 RFC 8439 ChaCha20-Poly1305 AEAD.
void chacha20_poly1305_encrypt(const CryptoKey256& key,
                               const Nonce96& nonce,
                               std::span<const std::uint8_t> aad,
                               std::span<const std::uint8_t> plaintext,
                               std::uint8_t* out_ciphertext,
                               AuthTag128& out_tag) noexcept;

[[nodiscard]] bool chacha20_poly1305_decrypt(const CryptoKey256& key,
                                             const Nonce96& nonce,
                                             std::span<const std::uint8_t> aad,
                                             std::span<const std::uint8_t> ciphertext,
                                             const AuthTag128& tag,
                                             std::uint8_t* out_plaintext) noexcept;

// Per-detector 256-bit key vault supporting explicit provisioning and SHA-256 KDF derivation.
class DetectorKeyVault {
public:
    void set_detector_key(std::uint32_t detector_id, const CryptoKey256& key);
    void derive_detector_key(const CryptoKey256& master_key, std::uint32_t detector_id);
    [[nodiscard]] Expected<CryptoKey256, RingStatus> get_key(
        std::uint32_t detector_id) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return keys_.size(); }

private:
    std::unordered_map<std::uint32_t, CryptoKey256> keys_{};
};

// Strictly order-preserving keyed encoding of finite IEEE-754 floats into uint64.
// Guarantee: for any finite floats x <= y, ope_encode_f32(x, k) <= ope_encode_f32(y, k).
[[nodiscard]] std::uint64_t ope_encode_f32(float value,
                                           const CryptoKey256& key) noexcept;

[[nodiscard]] Sha256Digest compute_flag_trapdoor(std::uint32_t quality_flags,
                                                 const CryptoKey256& key) noexcept;

struct EncryptedEventFrame {
    std::uint32_t detector_id{0};
    std::uint32_t reserved{0};
    std::uint64_t sequence_id{0};
    Nonce96 nonce{};
    std::array<std::uint8_t, sizeof(EventFrame)> ciphertext{};
    AuthTag128 tag{};
    std::uint64_t ope_energy_token{0};
    std::uint64_t ope_abs_timing_token{0};
    std::uint64_t ope_pileup_token{0};
    Sha256Digest flag_trapdoor{};
};

struct EncryptedTriggerTrapdoor {
    std::uint32_t detector_id{0};
    bool check_flags{true};
    std::uint64_t min_energy_tok{0};
    std::uint64_t max_energy_tok{0};
    std::uint64_t max_abs_timing_tok{0};
    std::uint64_t max_pileup_tok{0};
    Sha256Digest required_flag_trapdoor{};
};

[[nodiscard]] Expected<EncryptedEventFrame, RingStatus> encrypt_event_frame(
    const EventFrame& frame,
    const DetectorKeyVault& vault) noexcept;

[[nodiscard]] Expected<EventFrame, RingStatus> decrypt_event_frame(
    const EncryptedEventFrame& enc,
    const DetectorKeyVault& vault) noexcept;

[[nodiscard]] Expected<EncryptedTriggerTrapdoor, RingStatus> make_encrypted_trigger_trapdoor(
    std::uint32_t detector_id,
    const TriggerConfig& cfg,
    const DetectorKeyVault& vault,
    std::uint32_t expected_healthy_flags = (kFlagValid | kFlagCalibrated)) noexcept;

[[nodiscard]] bool evaluate_encrypted_trigger(
    const EncryptedEventFrame& enc,
    const EncryptedTriggerTrapdoor& trapdoor) noexcept;

} // namespace chronoring
