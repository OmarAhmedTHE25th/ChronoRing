#pragma once

#include "chronoring/status.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace chronoring {

using Sha256Digest = std::array<std::uint8_t, 32>;
using PqSeed256 = std::array<std::uint8_t, 32>;

inline constexpr std::size_t kWotsChains = 67; // 64 message nibbles + 3 checksum nibbles

[[nodiscard]] Sha256Digest sha256(const void* data, std::size_t len) noexcept;

[[nodiscard]] inline Sha256Digest sha256(std::span<const std::uint8_t> data) noexcept {
    return sha256(data.data(), data.size());
}

// RFC 2104 HMAC-SHA256.
[[nodiscard]] Sha256Digest hmac_sha256(std::span<const std::uint8_t> key,
                                       std::span<const std::uint8_t> data) noexcept;

// RFC 5869 HKDF-SHA256 Extract & Expand.
[[nodiscard]] Sha256Digest hkdf_sha256_extract(std::span<const std::uint8_t> salt,
                                               std::span<const std::uint8_t> ikm) noexcept;

[[nodiscard]] bool hkdf_sha256_expand(const Sha256Digest& prk,
                                      std::span<const std::uint8_t> info,
                                      std::span<std::uint8_t> okm) noexcept;

// Convenience one-shot RFC 5869 HKDF-SHA256 deriving a 32-byte key.
[[nodiscard]] Sha256Digest hkdf_sha256(std::span<const std::uint8_t> salt,
                                       std::span<const std::uint8_t> ikm,
                                       std::span<const std::uint8_t> info) noexcept;

// Best-effort volatile memory wipe for secret key material.
void secure_zero(void* data, std::size_t len) noexcept;

[[nodiscard]] std::string digest_to_hex(const Sha256Digest& digest);

[[nodiscard]] bool constant_time_equal(const Sha256Digest& a,
                                       const Sha256Digest& b) noexcept;

struct MerkleProof {
    std::vector<Sha256Digest> siblings{};
    std::vector<bool> sibling_is_right{};
};

// Append-only binary SHA-256 Merkle tree with domain-separated hashing:
//   leaf = SHA256(0x00 || data)
//   node = SHA256(0x01 || left || right)
class PageMerkleTree {
public:
    PageMerkleTree() = default;

    void clear() noexcept { leaves_.clear(); }
    [[nodiscard]] std::size_t leaf_count() const noexcept { return leaves_.size(); }
    [[nodiscard]] bool empty() const noexcept { return leaves_.empty(); }

    [[nodiscard]] static Sha256Digest hash_leaf(const void* data, std::size_t len) noexcept;
    [[nodiscard]] static Sha256Digest hash_node(const Sha256Digest& left,
                                                const Sha256Digest& right) noexcept;

    Sha256Digest append_page(const void* data, std::size_t len);
    Sha256Digest append_leaf_digest(const Sha256Digest& page_raw_digest);

    [[nodiscard]] Sha256Digest root() const;
    [[nodiscard]] Expected<MerkleProof, RingStatus> generate_proof(std::size_t leaf_idx) const;

    [[nodiscard]] static bool verify_proof(const Sha256Digest& leaf_hash,
                                           const MerkleProof& proof,
                                           const Sha256Digest& expected_root) noexcept;

private:
    std::vector<Sha256Digest> leaves_{};
};

// Quantum-resistant Winternitz One-Time Signature Plus (WOTS+, w=16, SHA-256,
// RFC 8391-style per-step keyed bitmask XOR) with 256-bit secret & public seeds.
//
// SECURITY CONTRACT:
//   WOTS+ is strictly a ONE-TIME signature scheme. Reusing a private key to sign
//   two distinct messages enables forgery. `PqPrivateKey` is therefore move-only
//   and `pq_sign` marks it consumed and wipes its secret chain values in place,
//   rejecting any subsequent signing attempt with `RingStatus::InvalidArgument`.
struct PqPublicKey {
    PqSeed256 pub_seed{};
    std::array<Sha256Digest, kWotsChains> pk{};
};

struct PqPrivateKey {
    PqSeed256 pub_seed{};
    std::array<Sha256Digest, kWotsChains> sk{};
    bool used{false};
    bool initialized{false};

    PqPrivateKey() noexcept = default;
    ~PqPrivateKey() noexcept;

    PqPrivateKey(const PqPrivateKey&) = delete;
    PqPrivateKey& operator=(const PqPrivateKey&) = delete;

    PqPrivateKey(PqPrivateKey&& other) noexcept;
    PqPrivateKey& operator=(PqPrivateKey&& other) noexcept;
};

struct PqKeyPair {
    PqPublicKey pub{};
    PqPrivateKey priv{};
};

struct PqSignature {
    std::array<Sha256Digest, kWotsChains> sig{};
};

// Generate a WOTS+ key pair from explicit 256-bit secret and public seeds.
[[nodiscard]] PqKeyPair pq_generate_keypair(const PqSeed256& secret_seed,
                                            const PqSeed256& pub_seed) noexcept;

// Generate a WOTS+ key pair from a single 256-bit seed by deriving domain-separated
// secret and public seeds via HKDF-SHA256.
[[nodiscard]] PqKeyPair pq_generate_keypair(const PqSeed256& seed_256) noexcept;

// Sign `msg` with `sk`, consuming and wiping `sk` so it cannot be reused.
// Returns `RingStatus::InvalidArgument` if `sk` is uninitialized or already used.
[[nodiscard]] Expected<PqSignature, RingStatus> pq_sign(const Sha256Digest& msg,
                                                        PqPrivateKey& sk) noexcept;

[[nodiscard]] bool pq_verify(const Sha256Digest& msg,
                             const PqSignature& sig,
                             const PqPublicKey& pk) noexcept;

} // namespace chronoring
