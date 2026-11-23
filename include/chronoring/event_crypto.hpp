#pragma once

#include "chronoring/crypto_integrity.hpp"
#include "chronoring/event_frame.hpp"
#include "chronoring/status.hpp"

#include <array>
#include <atomic>
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

// Per-detector 256-bit key vault supporting explicit provisioning and RFC 5869
// HKDF-SHA256 derivation. Derives cryptographically independent domain-separated
// subkeys for AEAD encryption and HMAC-SHA256 flag trapdoors so no single key is
// ever shared across purposes.
class DetectorKeyVault {
public:
    DetectorKeyVault() = default;
    ~DetectorKeyVault() noexcept;

    void set_detector_key(std::uint32_t detector_id, const CryptoKey256& root_key);
    void derive_detector_key(const CryptoKey256& master_key, std::uint32_t detector_id);

    [[nodiscard]] Expected<CryptoKey256, RingStatus> get_key(
        std::uint32_t detector_id) const noexcept;
    [[nodiscard]] Expected<CryptoKey256, RingStatus> get_aead_key(
        std::uint32_t detector_id) const noexcept;
    [[nodiscard]] Expected<CryptoKey256, RingStatus> get_trapdoor_key(
        std::uint32_t detector_id) const noexcept;

    [[nodiscard]] std::size_t size() const noexcept { return keys_.size(); }

private:
    std::unordered_map<std::uint32_t, CryptoKey256> keys_{};
};

// Computes an HMAC-SHA256 equality trapdoor over masked quality flags using a
// dedicated trapdoor subkey (never the AEAD encryption key).
[[nodiscard]] Sha256Digest compute_flag_trapdoor(std::uint32_t quality_flags,
                                                 const CryptoKey256& trapdoor_key) noexcept;

struct EncryptedEventFrame {
    std::uint32_t detector_id{0};
    std::uint32_t reserved{0};
    std::uint64_t sequence_id{0};
    Nonce96 nonce{};
    std::array<std::uint8_t, sizeof(EventFrame)> ciphertext{};
    AuthTag128 tag{};
    Sha256Digest flag_trapdoor{};
};

struct EncryptedFlagTrapdoor {
    std::uint32_t detector_id{0};
    Sha256Digest required_flag_trapdoor{};
};

// Stateful ChaCha20-Poly1305 event encryptor owning a monotonic 64-bit nonce counter
// paired with a 32-bit session prefix.
//
// SECURITY CONTRACT:
//   Nonces are NEVER derived from untrusted/repeatable frame fields such as
//   `frame.sequence_id`. Each `encrypt()` call atomically claims a strictly unique
//   counter value under `session_id_` and fails closed before wrap-around.
//   Across process restarts or concurrent producers sharing a detector key, callers
//   should rotate keys or ensure distinct `session_id` values per producer/session.
class DetectorEncryptor {
public:
    explicit DetectorEncryptor(const DetectorKeyVault& vault,
                               std::uint32_t session_id = generate_random_session_id()) noexcept;

    DetectorEncryptor(const DetectorEncryptor&) = delete;
    DetectorEncryptor& operator=(const DetectorEncryptor&) = delete;

    [[nodiscard]] Expected<EncryptedEventFrame, RingStatus> encrypt(
        const EventFrame& frame) noexcept;

    [[nodiscard]] std::uint32_t session_id() const noexcept { return session_id_; }
    [[nodiscard]] std::uint64_t next_counter() const noexcept {
        return counter_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] static std::uint32_t generate_random_session_id() noexcept;

private:
    const DetectorKeyVault* vault_;
    std::uint32_t session_id_;
    std::atomic<std::uint64_t> counter_{0};
};

[[nodiscard]] Expected<EventFrame, RingStatus> decrypt_event_frame(
    const EncryptedEventFrame& enc,
    const DetectorKeyVault& vault) noexcept;

[[nodiscard]] Expected<EncryptedFlagTrapdoor, RingStatus> make_encrypted_flag_trapdoor(
    std::uint32_t detector_id,
    const DetectorKeyVault& vault,
    std::uint32_t expected_healthy_flags = (kFlagValid | kFlagCalibrated)) noexcept;

[[nodiscard]] bool evaluate_encrypted_flag_trapdoor(
    const EncryptedEventFrame& enc,
    const EncryptedFlagTrapdoor& trapdoor) noexcept;

} // namespace chronoring
