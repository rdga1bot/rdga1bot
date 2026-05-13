#include <gtest/gtest.h>
#include "../src/Blackboard.h"

// ── Construction ──────────────────────────────────────────────────────────────

TEST(BlackboardTest, DefaultAllZero) {
    Blackboard bb;
    for (int i = 0; i < BB::FLOAT_COUNT; ++i)
        EXPECT_FLOAT_EQ(0.f, bb.getF(static_cast<BB::Float>(i)));
    for (int i = 0; i < BB::INT_COUNT; ++i)
        EXPECT_EQ(0, bb.getI(static_cast<BB::Int>(i)));
    for (int i = 0; i < BB::BOOL_COUNT; ++i)
        EXPECT_FALSE(bb.getB(static_cast<BB::Bool>(i)));
}

// ── Float ─────────────────────────────────────────────────────────────────────

TEST(BlackboardTest, FloatRoundtrip) {
    Blackboard bb;
    bb.setF(BB::Float::PLAYER_HP_PCT, 0.75f);
    EXPECT_FLOAT_EQ(0.75f, bb.getF(BB::Float::PLAYER_HP_PCT));
}

TEST(BlackboardTest, FloatSlotIndependent) {
    Blackboard bb;
    bb.setF(BB::Float::PLAYER_HP_PCT, 0.5f);
    bb.setF(BB::Float::PLAYER_MP_PCT, 0.9f);
    EXPECT_FLOAT_EQ(0.5f, bb.getF(BB::Float::PLAYER_HP_PCT));
    EXPECT_FLOAT_EQ(0.9f, bb.getF(BB::Float::PLAYER_MP_PCT));
}

TEST(BlackboardTest, FloatOverwrite) {
    Blackboard bb;
    bb.setF(BB::Float::KILLS_PER_MIN, 2.5f);
    bb.setF(BB::Float::KILLS_PER_MIN, 4.1f);
    EXPECT_FLOAT_EQ(4.1f, bb.getF(BB::Float::KILLS_PER_MIN));
}

// ── Int ───────────────────────────────────────────────────────────────────────

TEST(BlackboardTest, IntRoundtrip) {
    Blackboard bb;
    bb.setI(BB::Int::ALIVE_MOB_COUNT, 7);
    EXPECT_EQ(7, bb.getI(BB::Int::ALIVE_MOB_COUNT));
}

TEST(BlackboardTest, IntNegative) {
    Blackboard bb;
    bb.setI(BB::Int::DEAD_STREAK, -1);
    EXPECT_EQ(-1, bb.getI(BB::Int::DEAD_STREAK));
}

// ── Bool ─────────────────────────────────────────────────────────────────────

TEST(BlackboardTest, BoolSetTrue) {
    Blackboard bb;
    bb.setB(BB::Bool::HAVE_TARGET, true);
    EXPECT_TRUE(bb.getB(BB::Bool::HAVE_TARGET));
}

TEST(BlackboardTest, BoolSetFalse) {
    Blackboard bb;
    bb.setB(BB::Bool::HAVE_TARGET, true);
    bb.setB(BB::Bool::HAVE_TARGET, false);
    EXPECT_FALSE(bb.getB(BB::Bool::HAVE_TARGET));
}

TEST(BlackboardTest, BoolSlotsAreIndependent) {
    Blackboard bb;
    bb.setB(BB::Bool::HAVE_TARGET,  true);
    bb.setB(BB::Bool::IS_DEAD,      false);
    bb.setB(BB::Bool::CLOSE_THREAT, true);
    bb.setB(BB::Bool::FLEE_ACTIVE,  false);

    EXPECT_TRUE (bb.getB(BB::Bool::HAVE_TARGET));
    EXPECT_FALSE(bb.getB(BB::Bool::IS_DEAD));
    EXPECT_TRUE (bb.getB(BB::Bool::CLOSE_THREAT));
    EXPECT_FALSE(bb.getB(BB::Bool::FLEE_ACTIVE));
}

TEST(BlackboardTest, BoolAllSlots) {
    Blackboard bb;
    // Set all bool slots, verify each
    for (int i = 0; i < BB::BOOL_COUNT; ++i)
        bb.setB(static_cast<BB::Bool>(i), true);
    for (int i = 0; i < BB::BOOL_COUNT; ++i)
        EXPECT_TRUE(bb.getB(static_cast<BB::Bool>(i)));

    // Clear one, rest stay set
    bb.setB(BB::Bool::IN_GRACE, false);
    EXPECT_FALSE(bb.getB(BB::Bool::IN_GRACE));
    EXPECT_TRUE (bb.getB(BB::Bool::IS_DEAD));
    EXPECT_TRUE (bb.getB(BB::Bool::CLOSE_THREAT));
}

// ── Dump ─────────────────────────────────────────────────────────────────────

TEST(BlackboardTest, DumpContainsExpectedKeys) {
    Blackboard bb;
    bb.setF(BB::Float::PLAYER_HP_PCT, 0.8f);
    bb.setI(BB::Int::CURRENT_MOOD, 2);
    bb.setB(BB::Bool::FLEE_ACTIVE, true);

    std::string s = bb.dump();
    EXPECT_NE(std::string::npos, s.find("hp="));
    EXPECT_NE(std::string::npos, s.find("mood="));
    EXPECT_NE(std::string::npos, s.find("flee="));
}
