#include "chronoring/crypto_integrity.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace chronoring::test {

TEST(CryptoIntegrityTest, Sha256NistTestVectors) {
    const Sha256Digest empty_hash = sha256(nullptr, 0);
    EXPECT_EQ(digest_to_hex(empty_hash),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    const char* abc = "abc";
    const Sha256Digest abc_hash = sha256(abc, 3);
    EXPECT_EQ(digest_to_hex(abc_hash),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
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

TEST(CryptoIntegrityTest, PostQuantumWotsSignAndVerify) {
    const PqKeyPair kp = pq_generate_keypair(0xCAFEBABE12345678ULL);
    const char* msg_text = "ChronoRing_MerkleRoot_PageEpoch_42";
    const Sha256Digest msg = sha256(msg_text, 34);

    const PqSignature sig = pq_sign(msg, kp.priv);
    EXPECT_TRUE(pq_verify(msg, sig, kp.pub));

    // Message bit flip must fail
    Sha256Digest tampered_msg = msg;
    tampered_msg[10] ^= 0x04U;
    EXPECT_FALSE(pq_verify(tampered_msg, sig, kp.pub));

    // Signature bit flip must fail
    PqSignature tampered_sig = sig;
    tampered_sig.sig[3][5] ^= 0x02U;
    EXPECT_FALSE(pq_verify(msg, tampered_sig, kp.pub));

    // Wrong public key must fail
    const PqKeyPair other_kp = pq_generate_keypair(0x9999999999999999ULL);
    EXPECT_FALSE(pq_verify(msg, sig, other_kp.pub));
}

} // namespace chronoring::test
