/*
 * include/osw/control/jwt_verifier.h
 *
 * osw::control::JwtVerifier — minimal ES256 JWT verifier backed by
 * OpenSSL (linked transitively through gRPC's BoringSSL or system
 * OpenSSL).
 *
 * V1 contract:
 *   - Algorithm: ES256 (ECDSA-P256 + SHA-256). No other algorithm is
 *     accepted; a token with `"alg":"RS256"` (or any non-ES256 alg) is
 *     rejected with `kBadAlgorithm`.
 *   - Claims validated: `exp` (expiry, compared to current UTC time).
 *     `iss` and `aud` are NOT validated in V1 (per spec).
 *   - On success the `sub` claim is extracted and returned in
 *     `VerifyResult::subject`.
 *   - The public key is loaded once at construction from a PEM file.
 *     The loaded `EVP_PKEY*` is held via a custom deleter
 *     `shared_ptr`.
 *
 * Usage:
 *   auto v = JwtVerifier::FromPemFile("/etc/open_switch/jwt-es256.pub");
 *   if (!v) { ... load error ... }
 *   auto r = v->Verify("eyJ...");
 *   if (r.ok) { use r.subject; }
 *
 * Thread safety: all methods are const; multiple gRPC worker threads
 * may call `Verify()` concurrently on the same instance.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_CONTROL_JWT_VERIFIER_H_
#define OSW_CONTROL_JWT_VERIFIER_H_

#include <memory>
#include <string>
#include <string_view>

// Forward-declare OpenSSL type to avoid pulling <openssl/evp.h> into
// every header that includes this file.
struct evp_pkey_st;
using EVP_PKEY = evp_pkey_st;

namespace osw::control {

/// Result of a JWT verification attempt.
struct VerifyResult {
    bool ok = false;
    std::string subject;  ///< JWT `sub` claim. Non-empty iff ok.
    std::string error;    ///< Human-readable reason. Non-empty iff !ok.
};

/// Error codes returned by Verify() in VerifyResult::error prefix.
/// Prefix is the canonical key; the full error string adds detail.
struct JwtError {
    static constexpr std::string_view kBadFormat = "bad_format";
    static constexpr std::string_view kBadBase64 = "bad_base64";
    static constexpr std::string_view kBadJson = "bad_json";
    static constexpr std::string_view kBadAlgorithm = "bad_algorithm";
    static constexpr std::string_view kExpired = "expired";
    static constexpr std::string_view kBadSignature = "bad_signature";
    static constexpr std::string_view kMissingSubject = "missing_subject";
};

class JwtVerifier {
  public:
    /// Construct from an already-loaded `EVP_PKEY*` (takes ownership).
    explicit JwtVerifier(EVP_PKEY* pk) noexcept;
    ~JwtVerifier() noexcept;

    JwtVerifier(const JwtVerifier&) = delete;
    JwtVerifier& operator=(const JwtVerifier&) = delete;
    JwtVerifier(JwtVerifier&&) = delete;
    JwtVerifier& operator=(JwtVerifier&&) = delete;

    /// Load a PEM-encoded EC public key from `path` and return a
    /// JwtVerifier.  Returns nullptr on any error (file not found,
    /// malformed PEM, wrong key type).
    [[nodiscard]] static std::unique_ptr<JwtVerifier> FromPemFile(std::string_view path) noexcept;

    /// Load a PEM-encoded EC public key from a string (for unit tests).
    [[nodiscard]] static std::unique_ptr<JwtVerifier> FromPemString(std::string_view pem) noexcept;

    /// Verify a compact-serialised JWT ("header.payload.signature").
    /// Returns `VerifyResult{ok=true, subject=...}` on success.
    /// Returns `VerifyResult{ok=false, error=...}` on any failure.
    [[nodiscard]] VerifyResult Verify(std::string_view token) const noexcept;

  private:
    EVP_PKEY* pk_;  ///< Owned EC public key.
};

}  // namespace osw::control

#endif  // OSW_CONTROL_JWT_VERIFIER_H_
