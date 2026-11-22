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

inline constexpr std::size_t kWotsChains = 67; // 64 message nibbles + 3 checksum nibbles

[[nodiscard]] Sha256Digest sha256(const void* data, std::size_t len) noexcept;

[[nodiscard]] inline Sha256Digest sha256(std::span<const std::uint8_t> data) noexcept {
    return sha256(data.data(), data.size());
}

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

// Quantum-resistant Winternitz One-Time Signature (WOTS, w=16, SHA-256).
struct PqPublicKey {
    std::array<Sha256Digest, kWotsChains> pk{};
};

struct PqPrivateKey {
    std::array<Sha256Digest, kWotsChains> sk{};
};

struct PqKeyPair {
    PqPublicKey pub{};
    PqPrivateKey priv{};
};

struct PqSignature {
    std::array<Sha256Digest, kWotsChains> sig{};
};

[[nodiscard]] PqKeyPair pq_generate_keypair(std::uint64_t seed) noexcept;

[[nodiscard]] PqSignature pq_sign(const Sha256Digest& msg,
                                  const PqPrivateKey& sk) noexcept;

[[nodiscard]] bool pq_verify(const Sha256Digest& msg,
                             const PqSignature& sig,
                             const PqPublicKey& pk) noexcept;

} // namespace chronoring
