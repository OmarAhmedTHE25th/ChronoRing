#include "chronoring/crypto_integrity.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

namespace chronoring::test {

namespace {

std::string bytes_to_hex(std::span<const std::uint8_t> data) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(data.size() * 2);
    for (std::uint8_t b : data) {
        out.push_back(kHex[(b >> 4U) & 0x0FU]);
        out.push_back(kHex[b & 0x0FU]);
    }
    return out;
}

} // namespace

TEST(CryptoIntegrityTest, Sha256NistTestVectors) {
    const Sha256Digest empty_hash = sha256(nullptr, 0);
    EXPECT_EQ(digest_to_hex(empty_hash),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    const char* abc = "abc";
    const Sha256Digest abc_hash = sha256(abc, 3);
    EXPECT_EQ(digest_to_hex(abc_hash),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(CryptoIntegrityTest, HmacSha256Rfc4231KnownAnswerTests) {
    // RFC 4231 Test Case 1
    std::array<std::uint8_t, 20> key1{};
    key1.fill(0x0BU);
    const std::string data1 = "Hi There";
    const Sha256Digest mac1 = hmac_sha256(
        key1,
        std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(data1.data()), data1.size()));
    EXPECT_EQ(digest_to_hex(mac1),
              "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");

    // RFC 4231 Test Case 2
    const std::string key2 = "Jefe";
    const std::string data2 = "what do ya want for nothing?";
    const Sha256Digest mac2 = hmac_sha256(
        std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(key2.data()), key2.size()),
        std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(data2.data()), data2.size()));
    EXPECT_EQ(digest_to_hex(mac2),
              "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");

    // RFC 4231 Test Case 6 (key > 64-byte block size)
    std::array<std::uint8_t, 131> key6{};
    key6.fill(0xAAU);
    const std::string data6 = "Test Using Larger Than Block-Size Key - Hash Key First";
    const Sha256Digest mac6 = hmac_sha256(
        key6,
        std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(data6.data()), data6.size()));
    EXPECT_EQ(digest_to_hex(mac6),
              "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
}

TEST(CryptoIntegrityTest, HkdfSha256Rfc5869KnownAnswerTests) {
    // RFC 5869 Appendix A.1 (Basic test case with SHA-256)
    std::array<std::uint8_t, 22> ikm{};
    ikm.fill(0x0BU);
    const std::array<std::uint8_t, 13> salt{
        0x00U, 0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U,
        0x07U, 0x08U, 0x09U, 0x0AU, 0x0BU, 0x0CU};
    const std::array<std::uint8_t, 10> info{
        0xF0U, 0xF1U, 0xF2U, 0xF3U, 0xF4U, 0xF5U, 0xF6U, 0xF7U, 0xF8U, 0xF9U};

    const Sha256Digest prk1 = hkdf_sha256_extract(salt, ikm);
    EXPECT_EQ(digest_to_hex(prk1),
              "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5");

    std::array<std::uint8_t, 42> okm1{};
    ASSERT_TRUE(hkdf_sha256_expand(prk1, info, okm1));
    EXPECT_EQ(
        bytes_to_hex(okm1),
        "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865");

    // RFC 5869 Appendix A.3 (Zero-length salt and info)
    const Sha256Digest prk3 = hkdf_sha256_extract({}, ikm);
    EXPECT_EQ(digest_to_hex(prk3),
              "19ef24a32c717b167f33a91d6f648bdf96596776afdb6377ac434c1c293ccb04");

    std::array<std::uint8_t, 42> okm3{};
    ASSERT_TRUE(hkdf_sha256_expand(prk3, {}, okm3));
    EXPECT_EQ(
        bytes_to_hex(okm3),
        "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d9d201395faa4b61a96c8");
}

TEST(CryptoIntegrityTest, MerkleTreeProofsAndTamperRejection) {
    for (std::size_t count : {std::size_t{1}, std::size_t{2}, std::size_t{5}, std::size_t{17}}) {
        PageMerkleTree tree;
        std::vector<Sha256Digest> leaves;
        for (std::size_t i = 0; i < count; ++i) {
            const std::string payload = "ChronoRingPagePayload_" + std::to_string(i);
            leaves.push_back(tree.append_page(payload.data(), payload.size()));
        }
        EXPECT_EQ(tree.leaf_count(), count);
        const Sha256Digest root = tree.root();

        for (std::size_t i = 0; i < count; ++i) {
            const auto proof = tree.generate_proof(i);
            ASSERT_TRUE(proof.has_value());
            EXPECT_TRUE(PageMerkleTree::verify_proof(leaves[i], *proof, root));

            // 1-bit flip in leaf hash must fail verification
            Sha256Digest bad_leaf = leaves[i];
            bad_leaf[0] ^= 0x01U;
            EXPECT_FALSE(PageMerkleTree::verify_proof(bad_leaf, *proof, root));

            // 1-bit flip in root must fail verification
            Sha256Digest bad_root = root;
            bad_root[31] ^= 0x80U;
            EXPECT_FALSE(PageMerkleTree::verify_proof(leaves[i], *proof, bad_root));
        }

        EXPECT_FALSE(tree.generate_proof(count).has_value());
    }
}

TEST(CryptoIntegrityTest, PostQuantumWotsPlusSingleUseAndVerify) {
    static_assert(!std::is_copy_constructible_v<PqPrivateKey>);
    static_assert(!std::is_copy_assignable_v<PqPrivateKey>);

    const PqSeed256 seed1{
        0xCAU, 0xFEU, 0xBAU, 0xBEU, 0x12U, 0x34U, 0x56U, 0x78U,
        0x9AU, 0xBCU, 0xDEU, 0xF0U, 0x11U, 0x22U, 0x33U, 0x44U,
        0x55U, 0x66U, 0x77U, 0x88U, 0x99U, 0xAAU, 0xBBU, 0xCCU,
        0xDDU, 0xEEU, 0xFFU, 0x01U, 0x02U, 0x03U, 0x04U, 0x05U,
    };
    PqKeyPair kp = pq_generate_keypair(seed1);
    const char* msg_text = "ChronoRing_MerkleRoot_PageEpoch_42";
    const Sha256Digest msg = sha256(msg_text, 34);

    // Uninitialized private key must be rejected.
    PqPrivateKey uninit{};
    const auto uninit_res = pq_sign(msg, uninit);
    EXPECT_FALSE(uninit_res.has_value());
    EXPECT_EQ(uninit_res.error(), RingStatus::InvalidArgument);

    const auto sig_res = pq_sign(msg, kp.priv);
    ASSERT_TRUE(sig_res.has_value());
    const PqSignature& sig = *sig_res;
    EXPECT_TRUE(pq_verify(msg, sig, kp.pub));

    // One-time private key MUST refuse a second signature and wipe secret chain bytes.
    EXPECT_TRUE(kp.priv.used);
    for (const auto& chain : kp.priv.sk) {
        for (std::uint8_t byte : chain) {
            EXPECT_EQ(byte, 0U);
        }
    }
    const Sha256Digest msg2 = sha256("second_message", 14);
    const auto second_sig = pq_sign(msg2, kp.priv);
    EXPECT_FALSE(second_sig.has_value());
    EXPECT_EQ(second_sig.error(), RingStatus::InvalidArgument);

    // Message bit flip must fail
    Sha256Digest tampered_msg = msg;
    tampered_msg[10] ^= 0x04U;
    EXPECT_FALSE(pq_verify(tampered_msg, sig, kp.pub));

    // Signature bit flip must fail
    PqSignature tampered_sig = sig;
    tampered_sig.sig[3][5] ^= 0x02U;
    EXPECT_FALSE(pq_verify(msg, tampered_sig, kp.pub));

    // Public seed tamper must fail (WOTS+ bitmask binding)
    PqPublicKey tampered_pub = kp.pub;
    tampered_pub.pub_seed[0] ^= 0x01U;
    EXPECT_FALSE(pq_verify(msg, sig, tampered_pub));

    // Wrong public key must fail
    PqSeed256 seed2 = seed1;
    seed2[31] ^= 0xFFU;
    const PqKeyPair other_kp = pq_generate_keypair(seed2);
    EXPECT_FALSE(pq_verify(msg, sig, other_kp.pub));
}

} // namespace chronoring::test
