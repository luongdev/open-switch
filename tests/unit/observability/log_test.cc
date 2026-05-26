/*
 * tests/unit/observability/log_test.cc
 *
 * Unit tests for osw::log::*.
 *
 * The tests install a sink that captures emissions into a vector
 * (`TestSink::Records()`). They never call the real switch_log_printf.
 *
 * Covered:
 *   - Level / subsystem / traceparent passthrough.
 *   - printf-style argument substitution.
 *   - PII redaction:
 *       * Empty pattern list -> no change.
 *       * E.164 number (one regex case).
 *       * US-style 10-digit number (one regex case).
 *       * Custom config-supplied regex (one regex case).
 *       * Multiple patterns applied in sequence.
 *   - TraceScope: thread-local, nested.
 *   - Empty traceparent default.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/observability/log.h"

#include <gtest/gtest.h>

#include <mutex>
#include <regex>
#include <string>
#include <thread>
#include <vector>

namespace {

struct LogRecord {
    osw::log::Level level{osw::log::Level::kInfo};
    std::string     subsystem;
    std::string     traceparent;
    std::string     message;
};

class TestSink {
 public:
    static std::vector<LogRecord>& Records() {
        static std::vector<LogRecord> records;
        return records;
    }
    static std::mutex& Mu() {
        static std::mutex m;
        return m;
    }
    static void Sink(osw::log::Level level,
                     std::string_view subsystem,
                     std::string_view traceparent,
                     std::string_view message) noexcept {
        std::lock_guard<std::mutex> lk(Mu());
        Records().push_back(LogRecord{
            level,
            std::string(subsystem),
            std::string(traceparent),
            std::string(message),
        });
    }
    static void Clear() {
        std::lock_guard<std::mutex> lk(Mu());
        Records().clear();
    }
};

class LogTest : public ::testing::Test {
 protected:
    void SetUp() override {
        prev_sink_ = osw::log::SetSinkForTesting(&TestSink::Sink);
        osw::log::SetRedactionPatterns({});
        TestSink::Clear();
    }
    void TearDown() override {
        osw::log::SetSinkForTesting(prev_sink_);
        osw::log::SetRedactionPatterns({});
    }
    osw::log::SinkFn prev_sink_ = nullptr;
};

TEST_F(LogTest, LevelAndSubsystemPassThrough) {
    osw::log::Info("ctrl", "hello %d", 42);
    osw::log::Warn("evt", "warn line");

    auto& recs = TestSink::Records();
    ASSERT_EQ(recs.size(), 2u);
    EXPECT_EQ(recs[0].level, osw::log::Level::kInfo);
    EXPECT_EQ(recs[0].subsystem, "ctrl");
    EXPECT_EQ(recs[0].message, "hello 42");
    EXPECT_EQ(recs[1].level, osw::log::Level::kWarn);
    EXPECT_EQ(recs[1].subsystem, "evt");
    EXPECT_EQ(recs[1].message, "warn line");
}

TEST_F(LogTest, TraceScopeIsThreadLocalAndNested) {
    EXPECT_EQ(osw::log::CurrentTraceparent(), "");
    {
        osw::log::TraceScope outer("00-aabb-ccdd-01");
        EXPECT_EQ(osw::log::CurrentTraceparent(), "00-aabb-ccdd-01");
        osw::log::Info("core", "outer line");
        {
            osw::log::TraceScope inner("00-1111-2222-02");
            EXPECT_EQ(osw::log::CurrentTraceparent(), "00-1111-2222-02");
            osw::log::Info("core", "inner line");
        }
        EXPECT_EQ(osw::log::CurrentTraceparent(), "00-aabb-ccdd-01");
        osw::log::Info("core", "post-inner");
    }
    EXPECT_EQ(osw::log::CurrentTraceparent(), "");

    auto& recs = TestSink::Records();
    ASSERT_EQ(recs.size(), 3u);
    EXPECT_EQ(recs[0].traceparent, "00-aabb-ccdd-01");
    EXPECT_EQ(recs[1].traceparent, "00-1111-2222-02");
    EXPECT_EQ(recs[2].traceparent, "00-aabb-ccdd-01");
}

TEST_F(LogTest, TraceScopeDoesNotLeakAcrossThreads) {
    osw::log::TraceScope tp("main-tp");
    std::string seen_on_other;
    std::thread t([&]() { seen_on_other = std::string(osw::log::CurrentTraceparent()); });
    t.join();
    EXPECT_EQ(seen_on_other, "");
    EXPECT_EQ(osw::log::CurrentTraceparent(), "main-tp");
}

TEST_F(LogTest, EmptyPatternListLeavesMessageUntouched) {
    EXPECT_EQ(osw::log::ApplyRedactionForTesting("call +15551234567 hung up"),
              "call +15551234567 hung up");
}

TEST_F(LogTest, E164PatternRedactsPhoneNumber) {
    // E.164: optional '+', up to 15 digits.
    osw::log::SetRedactionPatterns({std::regex(R"(\+\d{8,15})")});
    EXPECT_EQ(osw::log::RedactionPatternCountForTesting(), 1u);
    EXPECT_EQ(osw::log::ApplyRedactionForTesting("dst=+84905555555 ok"),
              "dst=[REDACTED] ok");
}

TEST_F(LogTest, USTenDigitPatternRedactsNumber) {
    osw::log::SetRedactionPatterns({std::regex(R"(\b\d{3}-\d{3}-\d{4}\b)")});
    EXPECT_EQ(osw::log::ApplyRedactionForTesting("call 415-555-1212 from kiosk"),
              "call [REDACTED] from kiosk");
}

TEST_F(LogTest, CustomConfigPatternAppliesEvenIfPriorPatternMisses) {
    // Configurable custom pattern (e.g., tenant-specific SSN-like ID).
    osw::log::SetRedactionPatterns({
        std::regex(R"(SSN-\d{9})"),
        std::regex(R"(\bAC[A-Z]{3}\d{6}\b)"),  // account number shape
    });
    EXPECT_EQ(osw::log::RedactionPatternCountForTesting(), 2u);
    EXPECT_EQ(osw::log::ApplyRedactionForTesting("token=SSN-123456789 acct=ACABC987654 done"),
              "token=[REDACTED] acct=[REDACTED] done");
}

TEST_F(LogTest, RedactionAppliesToMessageBeforeSink) {
    osw::log::SetRedactionPatterns({std::regex(R"(\+\d{8,15})")});
    osw::log::Info("ctrl", "from=%s", "+15550006001");
    auto& recs = TestSink::Records();
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_EQ(recs[0].message, "from=[REDACTED]");
}

TEST_F(LogTest, LongFormattedMessageDoesNotTruncateBeyondHeap) {
    // 2KB of input characters via %s repeats — exercises the heap
    // fallback when stack buffer (1024) overflows.
    std::string huge(2000, 'X');
    osw::log::Info("evt", "begin=%s=end", huge.c_str());
    auto& recs = TestSink::Records();
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_NE(recs[0].message.find("XXXXXXXXXXXXXXXX"), std::string::npos);
    EXPECT_NE(recs[0].message.find("=end"), std::string::npos);
}

}  // namespace
