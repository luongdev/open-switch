/*
 * tests/unit/raii/xml_node_test.cc
 *
 * Unit tests for osw::XmlNode against the FS-mock seam.
 *
 * Covered:
 *   - Default-constructed is empty; destruction is a no-op.
 *   - open_cfg ctor with successful return stores root; destruction frees.
 *   - open_cfg ctor with failure leaves guard empty.
 *   - adopt() takes ownership without opening.
 *   - reset() / release() semantics.
 *   - Move-construction / move-assignment transfer ownership and free
 *     the destination's prior root.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/raii/xml_node.h"

#include <gtest/gtest.h>

#include "osw/raii/fs_mock.h"

namespace {

// Sentinel XML root handles. Mock never dereferences them.
switch_xml_t const kRootA = reinterpret_cast<switch_xml_t>(0x60A);
switch_xml_t const kRootB = reinterpret_cast<switch_xml_t>(0x60B);

class XmlNodeTest : public ::testing::Test {
 protected:
    void SetUp() override { osw::raii::fs::MockReset(); }
};

TEST_F(XmlNodeTest, DefaultConstructedIsEmpty) {
    osw::XmlNode node;
    EXPECT_FALSE(static_cast<bool>(node));
    EXPECT_EQ(node.root(), nullptr);
    EXPECT_EQ(osw::raii::fs::Mock().xml_free_calls.load(), 0);
}

TEST_F(XmlNodeTest, OpenCfgSuccessStoresRootAndFreesOnDestruction) {
    auto& m = osw::raii::fs::Mock();
    m.next_xml_root = kRootA;
    {
        osw::XmlNode node("open_switch.conf", nullptr);
        EXPECT_TRUE(static_cast<bool>(node));
        EXPECT_EQ(node.root(), kRootA);
        EXPECT_EQ(m.xml_open_cfg_calls.load(), 1);
        EXPECT_EQ(m.xml_free_calls.load(), 0);
    }
    EXPECT_EQ(m.xml_free_calls.load(), 1);
}

TEST_F(XmlNodeTest, OpenCfgFailureLeavesGuardEmpty) {
    auto& m = osw::raii::fs::Mock();
    m.next_xml_root = nullptr;
    {
        osw::XmlNode node("missing.conf", nullptr);
        EXPECT_FALSE(static_cast<bool>(node));
        EXPECT_EQ(m.xml_open_cfg_calls.load(), 1);
    }
    EXPECT_EQ(m.xml_free_calls.load(), 0);
}

TEST_F(XmlNodeTest, AdoptTakesOwnershipWithoutOpening) {
    auto& m = osw::raii::fs::Mock();
    {
        auto node = osw::XmlNode::adopt(kRootA);
        EXPECT_TRUE(static_cast<bool>(node));
        EXPECT_EQ(node.root(), kRootA);
        EXPECT_EQ(m.xml_open_cfg_calls.load(), 0);
    }
    EXPECT_EQ(m.xml_free_calls.load(), 1);
}

TEST_F(XmlNodeTest, AdoptOfNullYieldsEmptyGuard) {
    auto node = osw::XmlNode::adopt(nullptr);
    EXPECT_FALSE(static_cast<bool>(node));
    EXPECT_EQ(osw::raii::fs::Mock().xml_free_calls.load(), 0);
}

TEST_F(XmlNodeTest, ResetIsEagerAndIdempotent) {
    auto& m = osw::raii::fs::Mock();
    m.next_xml_root = kRootA;
    osw::XmlNode node("f.conf", nullptr);
    node.reset();
    EXPECT_FALSE(static_cast<bool>(node));
    EXPECT_EQ(m.xml_free_calls.load(), 1);

    node.reset();  // idempotent
    EXPECT_EQ(m.xml_free_calls.load(), 1);
}

TEST_F(XmlNodeTest, ReleaseHandsRootToCallerNoFree) {
    auto& m = osw::raii::fs::Mock();
    m.next_xml_root = kRootA;
    switch_xml_t got = nullptr;
    {
        osw::XmlNode node("f.conf", nullptr);
        got = node.release();
        EXPECT_EQ(got, kRootA);
        EXPECT_FALSE(static_cast<bool>(node));
    }
    EXPECT_EQ(m.xml_free_calls.load(), 0);
    EXPECT_EQ(got, kRootA);  // caller now owns it
}

TEST_F(XmlNodeTest, MoveConstructionTransfersOwnership) {
    auto& m = osw::raii::fs::Mock();
    m.next_xml_root = kRootA;
    osw::XmlNode a("f.conf", nullptr);
    osw::XmlNode b(std::move(a));
    EXPECT_FALSE(static_cast<bool>(a));   // NOLINT(*-use-after-move)
    EXPECT_TRUE(static_cast<bool>(b));
    EXPECT_EQ(b.root(), kRootA);
    EXPECT_EQ(m.xml_free_calls.load(), 0);
}

TEST_F(XmlNodeTest, MoveAssignmentFreesDestinationsPrior) {
    auto& m = osw::raii::fs::Mock();
    m.next_xml_root = kRootA;
    osw::XmlNode a("a.conf", nullptr);

    m.next_xml_root = kRootB;
    osw::XmlNode b("b.conf", nullptr);

    EXPECT_EQ(m.xml_free_calls.load(), 0);
    b = std::move(a);
    EXPECT_EQ(m.xml_free_calls.load(), 1);
    EXPECT_FALSE(static_cast<bool>(a));   // NOLINT(*-use-after-move)
    EXPECT_TRUE(static_cast<bool>(b));
    EXPECT_EQ(b.root(), kRootA);
}

TEST_F(XmlNodeTest, SelfMoveAssignmentIsSafe) {
    auto& m = osw::raii::fs::Mock();
    m.next_xml_root = kRootA;
    osw::XmlNode a("f.conf", nullptr);
    osw::XmlNode& alias = a;
    a = std::move(alias);  // tolerated; the helper guards self-move
    EXPECT_TRUE(static_cast<bool>(a));
    EXPECT_EQ(a.root(), kRootA);
    EXPECT_EQ(m.xml_free_calls.load(), 0);
}

}  // namespace
