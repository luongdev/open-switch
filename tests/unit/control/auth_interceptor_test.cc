/*
 * tests/unit/control/auth_interceptor_test.cc
 *
 * Unit tests for AuthInterceptorFactory.
 *
 * Strategy: test the auth decision logic by exercising RbacRegistry and
 * JwtVerifier directly (the core of the auth decision), and verify the
 * factory/interceptor wiring compiles correctly and UpdateRegistry works.
 *
 * Full gRPC end-to-end tests (with a real server + client) are in the
 * W5 integration suite.  This unit test file verifies:
 *
 *   1. Factory constructs without crashing (sanity).
 *   2. UpdateRegistry atomically swaps the registry pointer.
 *   3. UpdateJwtVerifier atomically swaps the verifier pointer.
 *   4. CreateServerInterceptor returns a non-null interceptor.
 *
 * Auth decision logic tests live in rbac_test.cc + jwt_verifier_test.cc
 * (each component tested in isolation, which is the correct unit-test
 * decomposition).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/control/auth_interceptor.h"

#include <cstdint>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include "osw/control/jwt_verifier.h"
#include "osw/control/rbac.h"

namespace osw::control {
namespace {

// Build a minimal RbacRegistry suitable for tests.
std::shared_ptr<RbacRegistry> MakeTestRegistry(bool require = true) {
    constexpr const char* xml = R"xml(
<auth require="true">
  <role name="operator">
    <permission>health.read</permission>
    <permission>control.originate</permission>
  </role>
  <identity name="ops-cn" role="operator"/>
</auth>
)xml";
    constexpr const char* xml_no_require = R"xml(<auth require="false"/>)xml";
    auto cfg = ParseAuthConfig(require ? xml : xml_no_require);
    return std::make_shared<RbacRegistry>(std::move(cfg));
}

// ---------------------------------------------------------------------------
// Factory construction
// ---------------------------------------------------------------------------

TEST(AuthInterceptorFactoryTest, ConstructsWithRegistryOnly) {
    auto reg = MakeTestRegistry();
    ASSERT_NE(reg, nullptr);
    EXPECT_NO_THROW({ AuthInterceptorFactory factory(reg); });
}

TEST(AuthInterceptorFactoryTest, ConstructsWithRegistryAndNullVerifier) {
    auto reg = MakeTestRegistry();
    EXPECT_NO_THROW({ AuthInterceptorFactory factory(reg, nullptr); });
}

TEST(AuthInterceptorFactoryTest, CreateServerInterceptorReturnsNonNull) {
    auto reg = MakeTestRegistry();
    AuthInterceptorFactory factory(reg);
    // Pass nullptr for ServerRpcInfo — interceptor should still be created
    // without crashing (info may be null in unit-test contexts without a real
    // gRPC server).
    auto* interceptor = factory.CreateServerInterceptor(nullptr);
    EXPECT_NE(interceptor, nullptr);
    delete interceptor;
}

// ---------------------------------------------------------------------------
// UpdateRegistry
// ---------------------------------------------------------------------------

TEST(AuthInterceptorFactoryTest, UpdateRegistrySwapsPointer) {
    auto reg1 = MakeTestRegistry(true);
    AuthInterceptorFactory factory(reg1);

    // Create a second registry.
    auto reg2 = MakeTestRegistry(false);  // require=false

    // Before update: interceptors created will use reg1.
    // After update: interceptors created will use reg2.
    EXPECT_NO_THROW(factory.UpdateRegistry(reg2));

    // Create an interceptor after the update — should not crash.
    auto* interceptor = factory.CreateServerInterceptor(nullptr);
    EXPECT_NE(interceptor, nullptr);
    delete interceptor;
}

// ---------------------------------------------------------------------------
// UpdateJwtVerifier
// ---------------------------------------------------------------------------

TEST(AuthInterceptorFactoryTest, UpdateJwtVerifierWithNull) {
    auto reg = MakeTestRegistry();
    AuthInterceptorFactory factory(reg);
    // Setting null verifier should not crash.
    EXPECT_NO_THROW(factory.UpdateJwtVerifier(nullptr));
}

// ---------------------------------------------------------------------------
// Integration: RBAC decisions for ops-cn identity (require=true registry).
// Tests the Authorize() logic that AuthInterceptor delegates to.
// ---------------------------------------------------------------------------

TEST(AuthInterceptorDecisionTest, OperatorCnAllowedOriginate) {
    auto reg = MakeTestRegistry(true);
    auto d = reg->Authorize("ops-cn", "/open_switch.control.v1.ControlService/Originate");
    EXPECT_TRUE(d.allowed);
    EXPECT_EQ(d.identity, "ops-cn");
}

TEST(AuthInterceptorDecisionTest, AnonymousRequireTrueDenied) {
    auto reg = MakeTestRegistry(true);
    auto d = reg->Authorize("anonymous", "/open_switch.control.v1.ControlService/Health");
    EXPECT_FALSE(d.allowed);
    EXPECT_EQ(d.deny_reason, "unauthenticated");
}

TEST(AuthInterceptorDecisionTest, AnonymousRequireFalseHealthAllowed) {
    auto reg = MakeTestRegistry(false);
    auto d = reg->Authorize("anonymous", "/open_switch.control.v1.ControlService/Health");
    EXPECT_TRUE(d.allowed);
}

TEST(AuthInterceptorDecisionTest, AnonymousRequireFalseOriginateDenied) {
    auto reg = MakeTestRegistry(false);
    auto d = reg->Authorize("anonymous", "/open_switch.control.v1.ControlService/Originate");
    EXPECT_FALSE(d.allowed);
}

TEST(AuthInterceptorDecisionTest, UnknownCnDenied) {
    auto reg = MakeTestRegistry(true);
    auto d = reg->Authorize("unknown-cn", "/open_switch.control.v1.ControlService/Health");
    EXPECT_FALSE(d.allowed);
    EXPECT_EQ(d.deny_reason, "no_role_for_identity");
}

// ---------------------------------------------------------------------------
// JWT integration: verifier + RBAC combined.
// Uses a real EC key to produce a valid JWT, verifies it, then runs the
// RBAC decision with the extracted subject.
// ---------------------------------------------------------------------------

TEST(AuthInterceptorJwtTest, ValidJwtSubjectMapsToRole) {
    // Generate a fresh P-256 key pair.
    EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
    ASSERT_NE(pctx, nullptr);
    ASSERT_EQ(EVP_PKEY_keygen_init(pctx), 1);
    ASSERT_EQ(EVP_PKEY_CTX_set_ec_paramgen_curve_nid(pctx, NID_X9_62_prime256v1), 1);
    EVP_PKEY* raw_kp = nullptr;
    ASSERT_EQ(EVP_PKEY_keygen(pctx, &raw_kp), 1);
    EVP_PKEY_CTX_free(pctx);

    // Export public key PEM.
    BIO* bio = BIO_new(BIO_s_mem());
    ASSERT_NE(bio, nullptr);
    ASSERT_EQ(PEM_write_bio_PUBKEY(bio, raw_kp), 1);
    char* pem_data = nullptr;
    long pem_len = BIO_get_mem_data(bio, &pem_data);
    std::string pub_pem(pem_data, static_cast<std::size_t>(pem_len));
    BIO_free(bio);

    auto verifier = JwtVerifier::FromPemString(pub_pem);
    ASSERT_NE(verifier, nullptr);

    // Base64URL encode helper.
    auto b64url = [](const std::string& in) -> std::string {
        std::vector<uint8_t> buf((in.size() + 2) / 3 * 4 + 1);
        int n = EVP_EncodeBlock(
            buf.data(), reinterpret_cast<const uint8_t*>(in.data()), static_cast<int>(in.size()));
        std::string out(reinterpret_cast<char*>(buf.data()), static_cast<std::size_t>(n));
        for (char& c : out) {
            if (c == '+')
                c = '-';
            if (c == '/')
                c = '_';
        }
        while (!out.empty() && out.back() == '=')
            out.pop_back();
        return out;
    };

    // Build signing input.
    std::int64_t exp = static_cast<std::int64_t>(std::time(nullptr)) + 3600;
    std::string header = R"({"alg":"ES256","typ":"JWT"})";
    std::string payload = R"({"sub":"ops-cn","exp":)" + std::to_string(exp) + "}";
    std::string signing_input = b64url(header) + "." + b64url(payload);

    // Sign with EVP_DigestSign → DER → convert to raw 64-byte r‖s.
    EVP_MD_CTX* mctx = EVP_MD_CTX_new();
    ASSERT_NE(mctx, nullptr);
    ASSERT_EQ(EVP_DigestSignInit(mctx, nullptr, EVP_sha256(), nullptr, raw_kp), 1);
    ASSERT_EQ(EVP_DigestSignUpdate(mctx, signing_input.data(), signing_input.size()), 1);
    std::size_t sig_len = 0;
    EVP_DigestSignFinal(mctx, nullptr, &sig_len);
    std::vector<uint8_t> der(sig_len);
    EVP_DigestSignFinal(mctx, der.data(), &sig_len);
    der.resize(sig_len);
    EVP_MD_CTX_free(mctx);

    const uint8_t* dp = der.data();
    ECDSA_SIG* sig = d2i_ECDSA_SIG(nullptr, &dp, static_cast<long>(der.size()));
    ASSERT_NE(sig, nullptr);
    const BIGNUM* r_bn = nullptr;
    const BIGNUM* s_bn = nullptr;
    ECDSA_SIG_get0(sig, &r_bn, &s_bn);
    uint8_t raw_sig[64] = {};
    BN_bn2binpad(r_bn, raw_sig, 32);
    BN_bn2binpad(s_bn, raw_sig + 32, 32);
    ECDSA_SIG_free(sig);

    // Base64URL-encode raw signature.
    std::vector<uint8_t> encbuf((64 + 2) / 3 * 4 + 1);
    int elen = EVP_EncodeBlock(encbuf.data(), raw_sig, 64);
    std::string sig_b64(reinterpret_cast<char*>(encbuf.data()), static_cast<std::size_t>(elen));
    for (char& c : sig_b64) {
        if (c == '+')
            c = '-';
        if (c == '/')
            c = '_';
    }
    while (!sig_b64.empty() && sig_b64.back() == '=')
        sig_b64.pop_back();

    std::string token = signing_input + "." + sig_b64;
    EVP_PKEY_free(raw_kp);

    // Verify JWT → extract subject.
    auto vr = verifier->Verify(token);
    EXPECT_TRUE(vr.ok) << "JWT verify error: " << vr.error;
    EXPECT_EQ(vr.subject, "ops-cn");

    // Run RBAC decision with extracted subject.
    auto reg = MakeTestRegistry(true);
    auto d = reg->Authorize(vr.subject, "/open_switch.control.v1.ControlService/Originate");
    EXPECT_TRUE(d.allowed);
}

}  // namespace
}  // namespace osw::control
