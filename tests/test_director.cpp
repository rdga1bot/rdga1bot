#include <gtest/gtest.h>
#include "../src/Blackboard.h"
#include "../src/BotMood.h"
#include "../src/DirectorSystem.h"

// ── MoodManager ───────────────────────────────────────────────────────────────

TEST(MoodManagerTest, DefaultNeutral) {
    MoodManager mm;
    EXPECT_EQ(BotMood::NEUTRAL, mm.current().mood);
}

TEST(MoodManagerTest, FleeImmediateOnCriticalHP) {
    MoodManager mm;
    // FLEE bypasses hysteresis — should switch on first evaluation
    MoodState ms = mm.evaluate(0.15f, 2.f, 0, 1, 5.f);
    EXPECT_EQ(BotMood::FLEE, ms.mood);
}

TEST(MoodManagerTest, FleeWhenSurroundedAndLowHP) {
    MoodManager mm;
    MoodState ms = mm.evaluate(0.30f, 1.f, 0, 5, 5.f);
    EXPECT_EQ(BotMood::FLEE, ms.mood);
}

TEST(MoodManagerTest, CautiousAfterHysteresis) {
    MoodManager mm;
    for (int i = 0; i < 3; ++i)
        mm.evaluate(0.35f, 1.f, 0, 1, 10.f);
    EXPECT_EQ(BotMood::CAUTIOUS, mm.current().mood);
}

TEST(MoodManagerTest, CautiousOnDeathStreak) {
    MoodManager mm;
    for (int i = 0; i < 3; ++i)
        mm.evaluate(0.80f, 2.f, 3, 1, 10.f);
    EXPECT_EQ(BotMood::CAUTIOUS, mm.current().mood);
}

TEST(MoodManagerTest, AggressiveOnHighKPM) {
    MoodManager mm;
    for (int i = 0; i < 3; ++i)
        mm.evaluate(0.90f, 4.f, 0, 5, 5.f);
    EXPECT_EQ(BotMood::AGGRESSIVE, mm.current().mood);
}

TEST(MoodManagerTest, SuspiciousOnNoKills) {
    MoodManager mm;
    for (int i = 0; i < 3; ++i)
        mm.evaluate(0.90f, 0.f, 0, 0, 120.f);
    EXPECT_EQ(BotMood::SUSPICIOUS, mm.current().mood);
}

TEST(MoodManagerTest, HysteresisPreventsSingleEvalSwitch) {
    MoodManager mm;
    // Establish AGGRESSIVE
    for (int i = 0; i < 3; ++i)
        mm.evaluate(0.90f, 5.f, 0, 5, 3.f);
    EXPECT_EQ(BotMood::AGGRESSIVE, mm.current().mood);

    // Single eval with different conditions — mood must NOT switch yet
    mm.evaluate(0.90f, 0.5f, 0, 1, 5.f);
    EXPECT_EQ(BotMood::AGGRESSIVE, mm.current().mood);
}

TEST(MoodManagerTest, IntensityInRange) {
    MoodManager mm;
    MoodState ms = mm.evaluate(0.15f, 0.f, 0, 0, 5.f);
    EXPECT_GE(ms.value, 0.f);
    EXPECT_LE(ms.value, 1.f);
}

TEST(MoodManagerTest, ResetRestoresNeutral) {
    MoodManager mm;
    mm.evaluate(0.10f, 0.f, 0, 0, 5.f); // → FLEE
    EXPECT_EQ(BotMood::FLEE, mm.current().mood);
    mm.reset();
    EXPECT_EQ(BotMood::NEUTRAL, mm.current().mood);
}

// ── MoodRewardScale ───────────────────────────────────────────────────────────

TEST(MoodRewardScaleTest, NeutralIsUnity) {
    auto s = getMoodRewardScale(BotMood::NEUTRAL);
    EXPECT_FLOAT_EQ(1.f, s.kill_scale);
    EXPECT_FLOAT_EQ(1.f, s.death_scale);
    EXPECT_FLOAT_EQ(0.f, s.rest_bonus);
}

TEST(MoodRewardScaleTest, AggressiveBoostsKill) {
    auto s = getMoodRewardScale(BotMood::AGGRESSIVE);
    EXPECT_GT(s.kill_scale, 1.f);
    EXPECT_LT(s.death_scale, 1.f);
}

TEST(MoodRewardScaleTest, CautiousHasRestBonus) {
    auto s = getMoodRewardScale(BotMood::CAUTIOUS);
    EXPECT_GT(s.rest_bonus, 0.f);
    EXPECT_GT(s.death_scale, 1.f);
}

TEST(MoodRewardScaleTest, FleeMaxDeathPenaltyAndMoveBonus) {
    auto s = getMoodRewardScale(BotMood::FLEE);
    EXPECT_EQ(2.f, s.death_scale);
    EXPECT_GT(s.move_bonus, 0.f);
    EXPECT_LT(s.kill_scale, 1.f);
}

// ── ZoneRecord ────────────────────────────────────────────────────────────────

TEST(ZoneRecordTest, ScoreKPMOverDeathRate) {
    ZoneRecord z;
    z.kpm = 3.f; z.death_rate = 0.f; z.samples = 10;
    EXPECT_FLOAT_EQ(3.f, z.score());
}

TEST(ZoneRecordTest, ScoreReducedByDeathRate) {
    ZoneRecord z;
    z.kpm = 3.f; z.death_rate = 2.f; z.samples = 10;
    EXPECT_FLOAT_EQ(1.f, z.score()); // 3/(1+2)=1
}

TEST(ZoneRecordTest, ScoreConfidenceLinear) {
    ZoneRecord z;
    z.kpm = 3.f; z.death_rate = 0.f; z.samples = 5;
    EXPECT_FLOAT_EQ(1.5f, z.score()); // 3 * 0.5 conf
}

TEST(ZoneRecordTest, ScoreConfidenceClampsAt10) {
    ZoneRecord z;
    z.kpm = 3.f; z.death_rate = 0.f; z.samples = 20;
    EXPECT_FLOAT_EQ(3.f, z.score()); // conf = 1.0 (clamped)
}

// ── DirectorSystem ────────────────────────────────────────────────────────────

TEST(DirectorSystemTest, InitialDirectiveFarmHere) {
    Blackboard     bb;
    DirectorSystem ds(bb);
    EXPECT_EQ(StrategicDirective::FARM_HERE, ds.currentDirective());
}

TEST(DirectorSystemTest, InitialMoodNeutral) {
    Blackboard     bb;
    DirectorSystem ds(bb);
    EXPECT_EQ(BotMood::NEUTRAL, ds.currentMood());
}

TEST(DirectorSystemTest, InitialFleeNotActive) {
    Blackboard bb;
    DirectorSystem ds(bb);
    EXPECT_FALSE(bb.getB(BB::Bool::FLEE_ACTIVE));
}

TEST(DirectorSystemTest, DirectiveNamesNonEmpty) {
    EXPECT_FALSE(directiveName(StrategicDirective::FARM_HERE).empty());
    EXPECT_FALSE(directiveName(StrategicDirective::FLEE).empty());
    EXPECT_FALSE(directiveName(StrategicDirective::REPOSITION).empty());
}

TEST(DirectorSystemTest, MoodNamesNonEmpty) {
    EXPECT_FALSE(moodName(BotMood::NEUTRAL).empty());
    EXPECT_FALSE(moodName(BotMood::FLEE).empty());
    EXPECT_FALSE(moodName(BotMood::AGGRESSIVE).empty());
}
