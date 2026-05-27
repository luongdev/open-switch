/*
 * tests/unit/control/call_cause_test.cc
 *
 * Unit tests for osw::control::CallCause against the FS-mock seam.
 *
 * Covered:
 *   - Known cause strings → correct switch_call_cause_t values.
 *   - Known switch_call_cause_t values → correct strings.
 *   - Empty / unknown strings → SWITCH_CAUSE_NORMAL_CLEARING.
 *   - "UNSPECIFIED" → SWITCH_CAUSE_NORMAL_CLEARING.
 *   - Unknown switch_call_cause_t → "UNSPECIFIED".
 *   - Round-trip: FromString(ToString(x)) == x for all table entries.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/control/call_cause.h"

#include <gtest/gtest.h>

#include "osw/raii/fs_mock.h"

namespace {

using osw::control::CallCause;

class CallCauseTest : public ::testing::Test {
  protected:
    void SetUp() override { osw::raii::fs::MockReset(); }
};

// ---------------------------------------------------------------------------
// FromString: known values
// ---------------------------------------------------------------------------

TEST_F(CallCauseTest, NormalClearingFromString) {
    EXPECT_EQ(CallCause::FromString("NORMAL_CLEARING"), SWITCH_CAUSE_NORMAL_CLEARING);
}

TEST_F(CallCauseTest, UserBusyFromString) {
    EXPECT_EQ(CallCause::FromString("USER_BUSY"), SWITCH_CAUSE_USER_BUSY);
}

TEST_F(CallCauseTest, NoAnswerFromString) {
    EXPECT_EQ(CallCause::FromString("NO_ANSWER"), SWITCH_CAUSE_NO_ANSWER);
}

TEST_F(CallCauseTest, CallRejectedFromString) {
    EXPECT_EQ(CallCause::FromString("CALL_REJECTED"), SWITCH_CAUSE_CALL_REJECTED);
}

TEST_F(CallCauseTest, UserNotRegisteredFromString) {
    EXPECT_EQ(CallCause::FromString("USER_NOT_REGISTERED"), SWITCH_CAUSE_USER_NOT_REGISTERED);
}

TEST_F(CallCauseTest, SystemShutdownFromString) {
    EXPECT_EQ(CallCause::FromString("SYSTEM_SHUTDOWN"), SWITCH_CAUSE_SYSTEM_SHUTDOWN);
}

TEST_F(CallCauseTest, SuccessFromString) {
    EXPECT_EQ(CallCause::FromString("SUCCESS"), SWITCH_CAUSE_SUCCESS);
}

// ---------------------------------------------------------------------------
// FromString: fallback cases
// ---------------------------------------------------------------------------

TEST_F(CallCauseTest, EmptyStringYieldsNormalClearing) {
    EXPECT_EQ(CallCause::FromString(""), SWITCH_CAUSE_NORMAL_CLEARING);
}

TEST_F(CallCauseTest, UnspecifiedStringYieldsNormalClearing) {
    EXPECT_EQ(CallCause::FromString("UNSPECIFIED"), SWITCH_CAUSE_NORMAL_CLEARING);
}

TEST_F(CallCauseTest, UnknownStringYieldsNormalClearing) {
    EXPECT_EQ(CallCause::FromString("NOT_A_REAL_CAUSE"), SWITCH_CAUSE_NORMAL_CLEARING);
}

TEST_F(CallCauseTest, LowercaseIsNotRecognised) {
    // The mapping is case-sensitive (uppercase only, matching FS convention).
    EXPECT_EQ(CallCause::FromString("normal_clearing"), SWITCH_CAUSE_NORMAL_CLEARING);
}

// ---------------------------------------------------------------------------
// ToString: known values
// ---------------------------------------------------------------------------

TEST_F(CallCauseTest, NormalClearingToString) {
    EXPECT_EQ(CallCause::ToString(SWITCH_CAUSE_NORMAL_CLEARING), "NORMAL_CLEARING");
}

TEST_F(CallCauseTest, UserBusyToString) {
    EXPECT_EQ(CallCause::ToString(SWITCH_CAUSE_USER_BUSY), "USER_BUSY");
}

TEST_F(CallCauseTest, NoAnswerToString) {
    EXPECT_EQ(CallCause::ToString(SWITCH_CAUSE_NO_ANSWER), "NO_ANSWER");
}

TEST_F(CallCauseTest, NoneToString) {
    EXPECT_EQ(CallCause::ToString(SWITCH_CAUSE_NONE), "NONE");
}

// ---------------------------------------------------------------------------
// ToString: fallback for unknown values
// ---------------------------------------------------------------------------

TEST_F(CallCauseTest, UnknownValueYieldsUnspecified) {
    // 9999 is not a valid switch_call_cause_t value.
    EXPECT_EQ(CallCause::ToString(static_cast<switch_call_cause_t>(9999)), "UNSPECIFIED");
}

// ---------------------------------------------------------------------------
// Round-trip tests
// ---------------------------------------------------------------------------

TEST_F(CallCauseTest, RoundTripNormalClearing) {
    const auto cause = CallCause::FromString("NORMAL_CLEARING");
    EXPECT_EQ(CallCause::ToString(cause), "NORMAL_CLEARING");
}

TEST_F(CallCauseTest, RoundTripUserBusy) {
    const auto cause = CallCause::FromString("USER_BUSY");
    EXPECT_EQ(CallCause::ToString(cause), "USER_BUSY");
}

TEST_F(CallCauseTest, RoundTripSystemShutdown) {
    const auto cause = CallCause::FromString("SYSTEM_SHUTDOWN");
    EXPECT_EQ(CallCause::ToString(cause), "SYSTEM_SHUTDOWN");
}

TEST_F(CallCauseTest, RoundTripBlindTransfer) {
    const auto cause = CallCause::FromString("BLIND_TRANSFER");
    EXPECT_EQ(CallCause::ToString(cause), "BLIND_TRANSFER");
}

TEST_F(CallCauseTest, RoundTripRejectAll) {
    const auto cause = CallCause::FromString("REJECT_ALL");
    EXPECT_EQ(CallCause::ToString(cause), "REJECT_ALL");
}

}  // namespace
