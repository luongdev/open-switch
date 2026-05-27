/*
 * tests/unit/core/config_test.cc
 *
 * Tests for the FS-agnostic Validate(Config). The FS-dependent loader
 * LoadConfigFromFile is NOT tested here — it requires a running FS
 * process and is exercised by the W5 integration suite.
 *
 * Per W1 contract §"core/config_test.cc":
 *   - happy path
 *   - malformed XML — covered by the loader, deferred to W5
 *   - missing file fallback to defaults — deferred (loader behaviour)
 *   - validation failures:
 *       ring size < 256
 *       TTL <= 0
 *       missing TLS key when cert set
 *       (plus the additional checks Validate enforces)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/core/config.h"

#include <gtest/gtest.h>

namespace {

osw::Config Defaults() {
    return osw::Config{};
}

TEST(ConfigValidateTest, HappyPathDefaultsAreValid) {
    const auto v = osw::Validate(Defaults());
    EXPECT_TRUE(v.ok);
    EXPECT_EQ(v.error, "");
}

TEST(ConfigValidateTest, RejectsEmptyListenAddress) {
    auto c = Defaults();
    c.grpc_listen_address.clear();
    const auto v = osw::Validate(c);
    EXPECT_FALSE(v.ok);
    EXPECT_NE(v.error.find("grpc_listen_address"), std::string::npos);
}

TEST(ConfigValidateTest, RejectsListenAddressWithoutColon) {
    auto c = Defaults();
    c.grpc_listen_address = "0.0.0.0";  // no port
    const auto v = osw::Validate(c);
    EXPECT_FALSE(v.ok);
    EXPECT_NE(v.error.find("':'"), std::string::npos);
}

TEST(ConfigValidateTest, RejectsTLSCertSetButKeyMissing) {
    auto c = Defaults();
    c.grpc_tls_cert_path = "/etc/ssl/cert.pem";
    // key empty
    const auto v = osw::Validate(c);
    EXPECT_FALSE(v.ok);
    EXPECT_NE(v.error.find("grpc_tls_key_path"), std::string::npos);
}

TEST(ConfigValidateTest, RejectsTLSCAWithoutCert) {
    auto c = Defaults();
    c.grpc_tls_ca_path = "/etc/ssl/ca.pem";  // mTLS without server cert
    const auto v = osw::Validate(c);
    EXPECT_FALSE(v.ok);
    EXPECT_NE(v.error.find("grpc_tls_ca_path"), std::string::npos);
}

TEST(ConfigValidateTest, AcceptsTLSCertPlusKey) {
    auto c = Defaults();
    c.grpc_tls_cert_path = "/etc/ssl/cert.pem";
    c.grpc_tls_key_path = "/etc/ssl/key.pem";
    const auto v = osw::Validate(c);
    EXPECT_TRUE(v.ok);
}

TEST(ConfigValidateTest, RejectsRingTier1Below256) {
    auto c = Defaults();
    c.event_ring_capacity_tier1 = 255;
    const auto v = osw::Validate(c);
    EXPECT_FALSE(v.ok);
    EXPECT_NE(v.error.find("event_ring_capacity_tier1"), std::string::npos);
}

TEST(ConfigValidateTest, RejectsRingTier2Below256) {
    auto c = Defaults();
    c.event_ring_capacity_tier2 = 100;
    const auto v = osw::Validate(c);
    EXPECT_FALSE(v.ok);
    EXPECT_NE(v.error.find("event_ring_capacity_tier2"), std::string::npos);
}

TEST(ConfigValidateTest, RejectsRingTier3Below256) {
    auto c = Defaults();
    c.event_ring_capacity_tier3 = 0;
    const auto v = osw::Validate(c);
    EXPECT_FALSE(v.ok);
    EXPECT_NE(v.error.find("event_ring_capacity_tier3"), std::string::npos);
}

TEST(ConfigValidateTest, RejectsSubscriberSendQueueTooSmall) {
    auto c = Defaults();
    c.subscriber_send_queue_capacity = 32;
    const auto v = osw::Validate(c);
    EXPECT_FALSE(v.ok);
    EXPECT_NE(v.error.find("subscriber_send_queue_capacity"), std::string::npos);
}

TEST(ConfigValidateTest, RejectsZeroMaxSubscribers) {
    auto c = Defaults();
    c.max_subscribers = 0;
    const auto v = osw::Validate(c);
    EXPECT_FALSE(v.ok);
    EXPECT_NE(v.error.find("max_subscribers"), std::string::npos);
}

TEST(ConfigValidateTest, RejectsZeroIdempotencyTTL) {
    auto c = Defaults();
    c.idempotency_ttl_seconds = 0;
    const auto v = osw::Validate(c);
    EXPECT_FALSE(v.ok);
    EXPECT_NE(v.error.find("idempotency_ttl_seconds"), std::string::npos);
}

TEST(ConfigValidateTest, RejectsIdempotencyCacheTooSmall) {
    auto c = Defaults();
    c.idempotency_cache_capacity = 1;
    const auto v = osw::Validate(c);
    EXPECT_FALSE(v.ok);
    EXPECT_NE(v.error.find("idempotency_cache_capacity"), std::string::npos);
}

TEST(ConfigValidateTest, RejectsZeroDrainTimeout) {
    auto c = Defaults();
    c.drain_timeout_seconds = 0;
    const auto v = osw::Validate(c);
    EXPECT_FALSE(v.ok);
    EXPECT_NE(v.error.find("drain_timeout_seconds"), std::string::npos);
}

TEST(ConfigValidateTest, RejectsZeroGrpcDrainDeadline) {
    auto c = Defaults();
    c.grpc_drain_deadline_seconds = 0;
    const auto v = osw::Validate(c);
    EXPECT_FALSE(v.ok);
    EXPECT_NE(v.error.find("grpc_drain_deadline_seconds"), std::string::npos);
}

TEST(ConfigValidateTest, RejectsUnknownLogLevel) {
    auto c = Defaults();
    c.log_level = "fatal";  // not in our set
    const auto v = osw::Validate(c);
    EXPECT_FALSE(v.ok);
    EXPECT_NE(v.error.find("log_level"), std::string::npos);
}

TEST(ConfigValidateTest, AcceptsAllValidLogLevels) {
    for (const char* level : {"trace", "debug", "info", "warn", "error", "crit"}) {
        auto c = Defaults();
        c.log_level = level;
        const auto v = osw::Validate(c);
        EXPECT_TRUE(v.ok);
        EXPECT_EQ(v.error, "");
    }
}

TEST(ConfigValidateTest, RejectsInvalidPIIRegex) {
    auto c = Defaults();
    c.pii_redaction_patterns = {"valid+pattern", "[unclosed-bracket"};
    const auto v = osw::Validate(c);
    EXPECT_FALSE(v.ok);
    EXPECT_NE(v.error.find("pii_redaction_patterns[1]"), std::string::npos);
}

TEST(ConfigValidateTest, AcceptsValidPIIRegexList) {
    auto c = Defaults();
    c.pii_redaction_patterns = {
        R"(\+\d{8,15})",
        R"(\b\d{3}-\d{3}-\d{4}\b)",
    };
    const auto v = osw::Validate(c);
    EXPECT_TRUE(v.ok);
}

TEST(ConfigValidateTest, SilenceDriverDefaultsAreValid) {
    auto c = Defaults();
    EXPECT_TRUE(c.silence_driver_enabled);
    EXPECT_EQ(200u, c.max_silence_drivers);
    const auto v = osw::Validate(c);
    EXPECT_TRUE(v.ok);
}

TEST(ConfigValidateTest, RejectsSilenceDriverCapBelowRange) {
    auto c = Defaults();
    c.max_silence_drivers = 0;
    const auto v = osw::Validate(c);
    EXPECT_FALSE(v.ok);
    EXPECT_NE(v.error.find("max_silence_drivers"), std::string::npos);
}

TEST(ConfigValidateTest, RejectsSilenceDriverCapAboveRange) {
    auto c = Defaults();
    c.max_silence_drivers = 5001;
    const auto v = osw::Validate(c);
    EXPECT_FALSE(v.ok);
    EXPECT_NE(v.error.find("max_silence_drivers"), std::string::npos);
}

}  // namespace
