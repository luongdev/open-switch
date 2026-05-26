/*
 * tests/unit/observability/audit_test.cc
 *
 * Unit tests for osw::audit::Emit against the FS-mock seam.
 *
 * Covered:
 *   - Emit allocates a CUSTOM event via switch_event_create_subclass
 *     (FF-020) with subclass "osw.audit.<name>".
 *   - Caller-supplied headers are forwarded via
 *     switch_event_add_header_string in order.
 *   - The Event-Subclass header is set automatically by FS (mock mirrors
 *     FF-020 by capturing it).
 *   - fire() returns success → guard becomes empty → no destroy call.
 *   - create-subclass failure leaves no event allocated and Emit returns
 *     false; no add_header / fire calls follow.
 *   - fire failure: guard's fire() still nulls the underlying ptr per
 *     FF-017, no destroy call follows.
 *   - Convenience overloads (initializer_list, no-headers) work.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/observability/audit.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "osw/raii/fs_mock.h"

namespace {

switch_event_t* const kEventA = reinterpret_cast<switch_event_t*>(0xA001);

class AuditTest : public ::testing::Test {
  protected:
    void SetUp() override { osw::raii::fs::MockReset(); }

    static const osw::raii::fs::MockState::CapturedEvent* CapturedFor(switch_event_t* ptr) {
        auto& m = osw::raii::fs::Mock();
        std::lock_guard<std::mutex> g(m.capture_mu);
        auto it = m.events_by_ptr.find(ptr);
        if (it == m.events_by_ptr.end())
            return nullptr;
        return &it->second;
    }

    static bool HasHeader(const osw::raii::fs::MockState::CapturedEvent& cap,
                          const std::string& name,
                          const std::string& value) {
        return std::any_of(cap.headers.begin(), cap.headers.end(), [&](const auto& kv) {
            return kv.first == name && kv.second == value;
        });
    }
};

TEST_F(AuditTest, EmitCreatesCustomEventWithDottedSubclass) {
    auto& m = osw::raii::fs::Mock();
    m.next_event = kEventA;

    const bool ok = osw::audit::Emit("module_loaded", {{"module_version", "0.1.0"}});
    EXPECT_TRUE(ok);

    EXPECT_EQ(m.event_create_subclass_calls.load(), 1);
    EXPECT_EQ(m.event_create_calls.load(), 0);
    EXPECT_EQ(m.event_fire_calls.load(), 1);
    EXPECT_EQ(m.event_destroy_calls.load(), 0);  // fire empties guard
    EXPECT_EQ(m.event_add_header_calls.load(), 1);

    const auto* cap = CapturedFor(kEventA);
    ASSERT_NE(cap, nullptr);
    EXPECT_EQ(cap->type, SWITCH_EVENT_CUSTOM);
    EXPECT_EQ(cap->subclass_name, "osw.audit.module_loaded");
    EXPECT_TRUE(cap->fired);
    EXPECT_TRUE(HasHeader(*cap, "Event-Subclass", "osw.audit.module_loaded"));
    EXPECT_TRUE(HasHeader(*cap, "module_version", "0.1.0"));
}

TEST_F(AuditTest, EmitWithNoHeadersStillFires) {
    auto& m = osw::raii::fs::Mock();
    m.next_event = kEventA;

    const bool ok = osw::audit::Emit("subscriber_connected");
    EXPECT_TRUE(ok);

    EXPECT_EQ(m.event_add_header_calls.load(), 0);  // no caller headers
    EXPECT_EQ(m.event_fire_calls.load(), 1);

    const auto* cap = CapturedFor(kEventA);
    ASSERT_NE(cap, nullptr);
    EXPECT_EQ(cap->subclass_name, "osw.audit.subscriber_connected");
    // FS adds Event-Subclass automatically (FF-020); mock mirrors.
    EXPECT_TRUE(HasHeader(*cap, "Event-Subclass", "osw.audit.subscriber_connected"));
}

TEST_F(AuditTest, MultipleHeadersPreserveOrder) {
    auto& m = osw::raii::fs::Mock();
    m.next_event = kEventA;

    EXPECT_TRUE(osw::audit::Emit(
        "subscriber_kicked", {{"reason", "queue_full"}, {"call_id", "abc-123"}, {"tier", "1"}}));

    EXPECT_EQ(m.event_add_header_calls.load(), 3);

    const auto* cap = CapturedFor(kEventA);
    ASSERT_NE(cap, nullptr);

    // Captured order: Event-Subclass (from FF-020), then our 3 in order.
    ASSERT_EQ(cap->headers.size(), 4u);
    EXPECT_EQ(cap->headers[0].first, "Event-Subclass");
    EXPECT_EQ(cap->headers[1].first, "reason");
    EXPECT_EQ(cap->headers[1].second, "queue_full");
    EXPECT_EQ(cap->headers[2].first, "call_id");
    EXPECT_EQ(cap->headers[2].second, "abc-123");
    EXPECT_EQ(cap->headers[3].first, "tier");
    EXPECT_EQ(cap->headers[3].second, "1");
}

TEST_F(AuditTest, CreateSubclassFailureSurfacesAsFalse) {
    auto& m = osw::raii::fs::Mock();
    m.next_event_create_subclass_status = SWITCH_STATUS_GENERR;
    m.next_event = nullptr;

    const bool ok = osw::audit::Emit("module_loaded", {{"k", "v"}});
    EXPECT_FALSE(ok);

    EXPECT_EQ(m.event_create_subclass_calls.load(), 1);
    EXPECT_EQ(m.event_add_header_calls.load(), 0);
    EXPECT_EQ(m.event_fire_calls.load(), 0);
    EXPECT_EQ(m.event_destroy_calls.load(), 0);
}

TEST_F(AuditTest, FireFailureSurfacesAsFalseAndNoLeak) {
    auto& m = osw::raii::fs::Mock();
    m.next_event = kEventA;
    m.next_event_fire_status = SWITCH_STATUS_GENERR;

    const bool ok = osw::audit::Emit("module_loaded");
    EXPECT_FALSE(ok);

    EXPECT_EQ(m.event_create_subclass_calls.load(), 1);
    EXPECT_EQ(m.event_fire_calls.load(), 1);
    // Per FF-017, fire still nulls the caller's slot even on the failure
    // path (FS internally destroys the event). EventGuard mirrors this:
    // the guard's internal ptr is set to nullptr inside fire(), so the
    // dtor does NOT call switch_event_destroy.
    EXPECT_EQ(m.event_destroy_calls.load(), 0);
}

TEST_F(AuditTest, EmptyNameIsRejected) {
    auto& m = osw::raii::fs::Mock();
    m.next_event = kEventA;

    EXPECT_FALSE(osw::audit::Emit(""));
    EXPECT_EQ(m.event_create_subclass_calls.load(), 0);
    EXPECT_EQ(m.event_fire_calls.load(), 0);
}

TEST_F(AuditTest, SubclassPrefixIsPublicConstant) {
    EXPECT_EQ(osw::audit::kSubclassPrefix, "osw.audit.");
}

}  // namespace
