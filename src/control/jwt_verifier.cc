/*
 * src/control/jwt_verifier.cc — ES256 JWT verifier (~150 lines core).
 *
 * Algorithm:
 *   1. Split compact JWT on '.' into header, payload, signature parts.
 *   2. Base64URL-decode each part.
 *   3. Parse header JSON for "alg": must be "ES256".
 *   4. Compute SHA-256 of the raw bytes "header_b64url.payload_b64url"
 *      (i.e., the original two dot-separated Base64URL-encoded parts,
 *      NOT re-encoded after decode).
 *   5. Convert raw 64-byte (r‖s) ECDSA signature → DER.
 *   6. Verify via EVP_DigestVerify.
 *   7. Parse payload JSON for "exp" (compare to time(nullptr)) and "sub".
 *
 * JSON parser: hand-rolled, minimal. No external JSON dep — the payload
 * schema is tiny (we only look at "alg", "exp", "sub").  The minimal
 * parser extracts string and integer values by key from a flat JSON object
 * with no nesting.
 *
 * Base64URL: RFC 4648 §5. Pad with '=' to multiple of 4 before
 * EVP_DecodeBlock / custom decode. '+' and '/' not used; '-' and '_' are.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/control/jwt_verifier.h"

#include <cstdint>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include "osw/observability/log.h"

namespace osw::control {

namespace {

constexpr const char* kSubsystem = "control.jwt";

// ---------------------------------------------------------------------------
// Base64URL decode (RFC 4648 §5).
// ---------------------------------------------------------------------------
std::vector<uint8_t> Base64UrlDecode(std::string_view input) {
    // Replace URL-safe chars and add padding.
    std::string s(input);
    for (char& c : s) {
        if (c == '-') c = '+';
        if (c == '_') c = '/';
    }
    while (s.size() % 4 != 0) {
        s += '=';
    }

    // Use OpenSSL EVP_DecodeBlock.
    std::vector<uint8_t> out(s.size());  // upper bound
    int len = EVP_DecodeBlock(out.data(),
                              reinterpret_cast<const uint8_t*>(s.data()),
                              static_cast<int>(s.size()));
    if (len < 0) {
        return {};
    }
    // EVP_DecodeBlock includes padding bytes — trim based on padding chars.
    std::size_t padding = 0;
    for (auto it = s.rbegin(); it != s.rend() && *it == '='; ++it) {
        ++padding;
    }
    out.resize(static_cast<std::size_t>(len) - padding);
    return out;
}

// ---------------------------------------------------------------------------
// Minimal JSON field extractor.
// Extracts a string value: `"key":"value"` or `"key": "value"`.
// Extracts an integer value: `"key":12345` or `"key": 12345`.
// ---------------------------------------------------------------------------
std::string JsonExtractString(std::string_view json, std::string_view key) {
    std::string needle;
    needle += '"';
    needle += key;
    needle += '"';

    auto pos = json.find(needle);
    if (pos == std::string_view::npos) return {};
    pos += needle.size();

    // Skip whitespace + colon.
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
    if (pos >= json.size() || json[pos] != ':') return {};
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
    if (pos >= json.size() || json[pos] != '"') return {};
    ++pos;

    // Find closing quote (no escape handling — V1 sub claims are simple).
    auto end = json.find('"', pos);
    if (end == std::string_view::npos) return {};
    return std::string(json.substr(pos, end - pos));
}

std::int64_t JsonExtractInt(std::string_view json, std::string_view key) {
    std::string needle;
    needle += '"';
    needle += key;
    needle += '"';

    auto pos = json.find(needle);
    if (pos == std::string_view::npos) return -1;
    pos += needle.size();

    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
    if (pos >= json.size() || json[pos] != ':') return -1;
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
    if (pos >= json.size()) return -1;

    std::int64_t v = 0;
    bool found = false;
    while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
        v = v * 10 + (json[pos] - '0');
        found = true;
        ++pos;
    }
    return found ? v : -1;
}

// ---------------------------------------------------------------------------
// Convert raw 64-byte ECDSA signature (r‖s, 32 bytes each) to DER.
// DER SEQUENCE { INTEGER r, INTEGER s }.
// Returns empty vector on error.
// ---------------------------------------------------------------------------
std::vector<uint8_t> RawToDerEcdsaSig(const std::vector<uint8_t>& raw) {
    if (raw.size() != 64) {
        return {};
    }

    BIGNUM* r = BN_bin2bn(raw.data(),      32, nullptr);
    BIGNUM* s = BN_bin2bn(raw.data() + 32, 32, nullptr);
    if (!r || !s) {
        BN_free(r);
        BN_free(s);
        return {};
    }

    ECDSA_SIG* sig = ECDSA_SIG_new();
    if (!sig) {
        BN_free(r);
        BN_free(s);
        return {};
    }
    // ECDSA_SIG_set0 takes ownership of r and s.
    if (!ECDSA_SIG_set0(sig, r, s)) {
        BN_free(r);
        BN_free(s);
        ECDSA_SIG_free(sig);
        return {};
    }
    // r and s now owned by sig.

    int der_len = i2d_ECDSA_SIG(sig, nullptr);
    if (der_len <= 0) {
        ECDSA_SIG_free(sig);
        return {};
    }
    std::vector<uint8_t> der(static_cast<std::size_t>(der_len));
    uint8_t* p = der.data();
    i2d_ECDSA_SIG(sig, &p);
    ECDSA_SIG_free(sig);
    return der;
}

// ---------------------------------------------------------------------------
// EVP_DigestVerify wrapper for ES256.
// Returns true iff signature over msg is valid for pk.
// ---------------------------------------------------------------------------
bool EvpVerifyEs256(EVP_PKEY* pk,
                   const uint8_t* msg, std::size_t msg_len,
                   const uint8_t* sig_der, std::size_t sig_len) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return false;

    bool ok = false;
    if (EVP_DigestVerifyInit(ctx, nullptr, EVP_sha256(), nullptr, pk) == 1) {
        if (EVP_DigestVerifyUpdate(ctx, msg, msg_len) == 1) {
            ok = (EVP_DigestVerifyFinal(ctx, sig_der, sig_len) == 1);
        }
    }
    EVP_MD_CTX_free(ctx);
    return ok;
}

// Load EVP_PKEY from PEM data in memory.
EVP_PKEY* LoadPubKeyFromPem(const char* pem_data, std::size_t pem_len) {
    BIO* bio = BIO_new_mem_buf(pem_data, static_cast<int>(pem_len));
    if (!bio) return nullptr;
    EVP_PKEY* pk = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    return pk;
}

}  // namespace

// ---------------------------------------------------------------------------
// JwtVerifier
// ---------------------------------------------------------------------------

JwtVerifier::JwtVerifier(EVP_PKEY* pk) noexcept : pk_(pk) {}

JwtVerifier::~JwtVerifier() noexcept {
    if (pk_) {
        EVP_PKEY_free(pk_);
        pk_ = nullptr;
    }
}

// static
std::unique_ptr<JwtVerifier> JwtVerifier::FromPemFile(std::string_view path) noexcept {
    try {
        FILE* f = ::fopen(std::string(path).c_str(), "r");
        if (!f) {
            osw::log::Error(kSubsystem, "JwtVerifier::FromPemFile: cannot open '%.*s'",
                            static_cast<int>(path.size()), path.data());
            return nullptr;
        }
        BIO* bio = BIO_new_fp(f, BIO_CLOSE);
        if (!bio) {
            ::fclose(f);
            return nullptr;
        }
        EVP_PKEY* pk = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
        BIO_free(bio);
        if (!pk) {
            osw::log::Error(kSubsystem, "JwtVerifier::FromPemFile: PEM_read_bio_PUBKEY failed for '%.*s'",
                            static_cast<int>(path.size()), path.data());
            return nullptr;
        }
        return std::make_unique<JwtVerifier>(pk);
    } catch (...) {
        return nullptr;
    }
}

// static
std::unique_ptr<JwtVerifier> JwtVerifier::FromPemString(std::string_view pem) noexcept {
    try {
        EVP_PKEY* pk = LoadPubKeyFromPem(pem.data(), pem.size());
        if (!pk) {
            osw::log::Error(kSubsystem, "JwtVerifier::FromPemString: PEM_read_bio_PUBKEY failed");
            return nullptr;
        }
        return std::make_unique<JwtVerifier>(pk);
    } catch (...) {
        return nullptr;
    }
}

VerifyResult JwtVerifier::Verify(std::string_view token) const noexcept {
    try {
        // 1. Split on '.'.
        auto d1 = token.find('.');
        if (d1 == std::string_view::npos) {
            return {false, {}, std::string(JwtError::kBadFormat) + ":missing_dot1"};
        }
        auto d2 = token.find('.', d1 + 1);
        if (d2 == std::string_view::npos) {
            return {false, {}, std::string(JwtError::kBadFormat) + ":missing_dot2"};
        }

        std::string_view header_b64  = token.substr(0, d1);
        std::string_view payload_b64 = token.substr(d1 + 1, d2 - d1 - 1);
        std::string_view sig_b64     = token.substr(d2 + 1);

        // The signing input is the raw ASCII bytes "header_b64.payload_b64".
        std::string signing_input;
        signing_input.reserve(header_b64.size() + 1 + payload_b64.size());
        signing_input.append(header_b64);
        signing_input += '.';
        signing_input.append(payload_b64);

        // 2. Base64URL-decode header and payload.
        auto header_bytes  = Base64UrlDecode(header_b64);
        auto payload_bytes = Base64UrlDecode(payload_b64);
        auto sig_bytes     = Base64UrlDecode(sig_b64);

        if (header_bytes.empty()) {
            return {false, {}, std::string(JwtError::kBadBase64) + ":header"};
        }
        if (payload_bytes.empty()) {
            return {false, {}, std::string(JwtError::kBadBase64) + ":payload"};
        }
        if (sig_bytes.empty()) {
            return {false, {}, std::string(JwtError::kBadBase64) + ":signature"};
        }

        // 3. Check alg in header JSON.
        std::string header_json(reinterpret_cast<char*>(header_bytes.data()),
                                header_bytes.size());
        std::string alg = JsonExtractString(header_json, "alg");
        if (alg != "ES256") {
            return {false, {}, std::string(JwtError::kBadAlgorithm) + ":" + alg};
        }

        // 4. Parse payload JSON.
        std::string payload_json(reinterpret_cast<char*>(payload_bytes.data()),
                                 payload_bytes.size());

        std::int64_t exp = JsonExtractInt(payload_json, "exp");
        if (exp > 0) {
            std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
            if (now > exp) {
                return {false, {}, std::string(JwtError::kExpired)};
            }
        }

        std::string subject = JsonExtractString(payload_json, "sub");
        if (subject.empty()) {
            return {false, {}, std::string(JwtError::kMissingSubject)};
        }

        // 5. Convert raw signature to DER.
        auto sig_der = RawToDerEcdsaSig(sig_bytes);
        if (sig_der.empty()) {
            return {false, {}, std::string(JwtError::kBadSignature) + ":der_convert"};
        }

        // 6. Verify signature.
        bool valid = EvpVerifyEs256(
            pk_,
            reinterpret_cast<const uint8_t*>(signing_input.data()),
            signing_input.size(),
            sig_der.data(),
            sig_der.size());

        if (!valid) {
            return {false, {}, std::string(JwtError::kBadSignature) + ":verify_failed"};
        }

        return {true, std::move(subject), {}};
    } catch (const std::exception& e) {
        osw::log::Error(kSubsystem, "JwtVerifier::Verify threw: %s", e.what());
        return {false, {}, std::string(JwtError::kBadFormat) + ":exception"};
    } catch (...) {
        return {false, {}, std::string(JwtError::kBadFormat) + ":unknown_exception"};
    }
}

}  // namespace osw::control
