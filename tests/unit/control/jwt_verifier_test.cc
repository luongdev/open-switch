/*
 * tests/unit/control/jwt_verifier_test.cc
 *
 * Unit tests for JwtVerifier (ES256).
 *
 * Tests cover:
 *   - Valid ES256 JWT → ok=true, subject extracted.
 *   - Expired JWT (exp in the past) → kExpired.
 *   - Tampered payload → kBadSignature.
 *   - Wrong algorithm (RS256 in header) → kBadAlgorithm.
 *   - Missing "sub" claim → kMissingSubject.
 *   - Malformed token (no dots) → kBadFormat.
 *   - FromPemString with bad PEM → nullptr.
 *
 * Key generation: done inline via OpenSSL EC_KEY_new_by_curve_name so
 * the test is self-contained with no fixture files.
 *
 * JWT construction helper: minimal Base64URL encoder + JSON builder.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/control/jwt_verifier.h"

#include <cstdint>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

namespace osw::control {
namespace {

// ---------------------------------------------------------------------------
// Helpers: Base64URL encode and minimal JWT builder.
// ---------------------------------------------------------------------------

std::string Base64UrlEncode(const uint8_t* data, std::size_t len) {
    // Use EVP_EncodeBlock, then convert +/ to -_ and strip =.
    std::vector<uint8_t> buf((len + 2) / 3 * 4 + 1);
    int n = EVP_EncodeBlock(buf.data(), data, static_cast<int>(len));
    std::string s(reinterpret_cast<char*>(buf.data()), static_cast<std::size_t>(n));
    for (char& c : s) {
        if (c == '+') c = '-';
        if (c == '/') c = '_';
    }
    while (!s.empty() && s.back() == '=') s.pop_back();
    return s;
}

std::string Base64UrlEncodeStr(std::string_view sv) {
    return Base64UrlEncode(reinterpret_cast<const uint8_t*>(sv.data()), sv.size());
}

// Build a compact JWT with the given header JSON, payload JSON, signed by pk.
// Returns "" on OpenSSL error.
std::string BuildJwt(const std::string& header_json,
                     const std::string& payload_json,
                     EVP_PKEY* priv_key) {
    std::string h = Base64UrlEncodeStr(header_json);
    std::string p = Base64UrlEncodeStr(payload_json);
    std::string signing_input = h + "." + p;

    // EVP_DigestSign for ES256.
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return {};

    if (EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr, priv_key) != 1) {
        EVP_MD_CTX_free(ctx);
        return {};
    }
    if (EVP_DigestSignUpdate(ctx,
                             signing_input.data(),
                             signing_input.size()) != 1) {
        EVP_MD_CTX_free(ctx);
        return {};
    }

    // DER-encoded signature.
    std::size_t sig_len = 0;
    EVP_DigestSignFinal(ctx, nullptr, &sig_len);
    std::vector<uint8_t> der(sig_len);
    EVP_DigestSignFinal(ctx, der.data(), &sig_len);
    der.resize(sig_len);
    EVP_MD_CTX_free(ctx);

    // Parse DER → ECDSA_SIG → r‖s raw 64 bytes.
    const uint8_t* p2 = der.data();
    ECDSA_SIG* sig = d2i_ECDSA_SIG(nullptr, &p2, static_cast<long>(der.size()));
    if (!sig) return {};

    const BIGNUM* r = nullptr;
    const BIGNUM* s = nullptr;
    ECDSA_SIG_get0(sig, &r, &s);

    uint8_t raw[64] = {};
    BN_bn2binpad(r, raw,      32);
    BN_bn2binpad(s, raw + 32, 32);
    ECDSA_SIG_free(sig);

    std::string sig_b64 = Base64UrlEncode(raw, 64);
    return signing_input + "." + sig_b64;
}

// ---------------------------------------------------------------------------
// Test fixture: generates a fresh EC P-256 key pair per test.
// ---------------------------------------------------------------------------

class JwtVerifierTest : public ::testing::Test {
  protected:
    void SetUp() override {
        // Generate EC P-256 key pair.
        EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
        ASSERT_NE(pctx, nullptr);
        ASSERT_EQ(EVP_PKEY_keygen_init(pctx), 1);
        ASSERT_EQ(EVP_PKEY_CTX_set_ec_paramgen_curve_nid(pctx, NID_X9_62_prime256v1), 1);
        EVP_PKEY* raw_kp = nullptr;
        ASSERT_EQ(EVP_PKEY_keygen(pctx, &raw_kp), 1);
        EVP_PKEY_CTX_free(pctx);
        key_pair_.reset(raw_kp);

        // Write public key to PEM string.
        BIO* bio = BIO_new(BIO_s_mem());
        ASSERT_NE(bio, nullptr);
        ASSERT_EQ(PEM_write_bio_PUBKEY(bio, key_pair_.get()), 1);
        char* pem_data = nullptr;
        long pem_len = BIO_get_mem_data(bio, &pem_data);
        pub_pem_ = std::string(pem_data, static_cast<std::size_t>(pem_len));
        BIO_free(bio);

        verifier_ = JwtVerifier::FromPemString(pub_pem_);
        ASSERT_NE(verifier_, nullptr);
    }

    struct EvpKeyDeleter {
        void operator()(EVP_PKEY* p) const noexcept { EVP_PKEY_free(p); }
    };
    std::unique_ptr<EVP_PKEY, EvpKeyDeleter> key_pair_;
    std::string pub_pem_;
    std::unique_ptr<JwtVerifier> verifier_;

    // Build a valid ES256 JWT with the given sub and exp offset (seconds from now).
    std::string MakeJwt(const std::string& sub, std::int64_t exp_offset_secs = 3600) {
        std::int64_t exp = static_cast<std::int64_t>(std::time(nullptr)) + exp_offset_secs;
        std::string header  = R"({"alg":"ES256","typ":"JWT"})";
        std::string payload = R"({"sub":")" + sub + R"(","exp":)" + std::to_string(exp) + "}";
        return BuildJwt(header, payload, key_pair_.get());
    }
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_F(JwtVerifierTest, ValidJwtOk) {
    auto token = MakeJwt("test-user");
    ASSERT_FALSE(token.empty());

    auto r = verifier_->Verify(token);
    EXPECT_TRUE(r.ok) << "error: " << r.error;
    EXPECT_EQ(r.subject, "test-user");
    EXPECT_TRUE(r.error.empty());
}

TEST_F(JwtVerifierTest, ValidJwtSubjectPreserved) {
    auto token = MakeJwt("ops-team-cn");
    auto r = verifier_->Verify(token);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.subject, "ops-team-cn");
}

TEST_F(JwtVerifierTest, ExpiredJwt) {
    // exp = 1 second in the past.
    auto token = MakeJwt("user", -1);
    ASSERT_FALSE(token.empty());

    auto r = verifier_->Verify(token);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("expired"), std::string::npos) << "error was: " << r.error;
}

TEST_F(JwtVerifierTest, TamperedPayloadBadSignature) {
    auto token = MakeJwt("user");
    ASSERT_FALSE(token.empty());

    // Flip a character in the payload segment.
    auto d1 = token.find('.');
    auto d2 = token.find('.', d1 + 1);
    std::string tampered = token;
    // Flip a character in the payload (after first dot, before second dot).
    if (d1 + 2 < d2) {
        tampered[d1 + 2] ^= 0x01;
    }

    auto r = verifier_->Verify(tampered);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("bad_signature"), std::string::npos)
        << "error was: " << r.error;
}

TEST_F(JwtVerifierTest, WrongAlgorithmRejected) {
    // Build a token with alg=RS256 in the header.
    std::int64_t exp = static_cast<std::int64_t>(std::time(nullptr)) + 3600;
    std::string header  = R"({"alg":"RS256","typ":"JWT"})";
    std::string payload = R"({"sub":"user","exp":)" + std::to_string(exp) + "}";
    auto token = BuildJwt(header, payload, key_pair_.get());
    ASSERT_FALSE(token.empty());

    auto r = verifier_->Verify(token);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("bad_algorithm"), std::string::npos)
        << "error was: " << r.error;
}

TEST_F(JwtVerifierTest, MissingSubClaim) {
    std::int64_t exp = static_cast<std::int64_t>(std::time(nullptr)) + 3600;
    std::string header  = R"({"alg":"ES256","typ":"JWT"})";
    std::string payload = R"({"exp":)" + std::to_string(exp) + "}";
    auto token = BuildJwt(header, payload, key_pair_.get());
    ASSERT_FALSE(token.empty());

    auto r = verifier_->Verify(token);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("missing_subject"), std::string::npos)
        << "error was: " << r.error;
}

TEST_F(JwtVerifierTest, MalformedTokenNoDots) {
    auto r = verifier_->Verify("notavalidjwtatall");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("bad_format"), std::string::npos)
        << "error was: " << r.error;
}

TEST_F(JwtVerifierTest, MalformedTokenOneDot) {
    auto r = verifier_->Verify("header.payload");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("bad_format"), std::string::npos)
        << "error was: " << r.error;
}

TEST(JwtVerifierFromPemTest, BadPemReturnsNullptr) {
    auto v = JwtVerifier::FromPemString("not a pem");
    EXPECT_EQ(v, nullptr);
}

TEST(JwtVerifierFromPemTest, EmptyPemReturnsNullptr) {
    auto v = JwtVerifier::FromPemString("");
    EXPECT_EQ(v, nullptr);
}

}  // namespace
}  // namespace osw::control
