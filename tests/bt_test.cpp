// SPDX-License-Identifier: MIT
// BehaviorTree VM unit tests — Part 4 hardening
#include <gtest/gtest.h>
#include "../src/BehaviorTree.h"
#include "../src/game_state.h"
#include "../src/Eyes.h"
#include "../src/Hands.h"
#include "../src/Config.h"
#include "../src/Stats.h"

// ── GameState fixture ─────────────────────────────────────────────────────────
// Допоміжна структура що тримає стубові інструменти в живих на час тесту.
struct GsFixture {
    Eyes   eyes{};
    Hands  hands{};
    Config cfg{};
    Stats  stats{};
    GameState gs{eyes, hands, cfg, stats};
};

// ── Статичні Action/Condition для тестів ──────────────────────────────────────
static BTStatus act_success(GameState&) { return BTStatus::Success; }
static BTStatus act_failure(GameState&) { return BTStatus::Failure; }

static uint32_t now_ms() {
    return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ── Тест 1: Selector повертає Success при першому Success дочірньому ──────────
TEST(BehaviorTreeTest, SelectorSuccessOnFirstChild) {
    BehaviorTree bt;
    uint16_t sel  = bt.addSelector();
    uint16_t act1 = bt.addAction(act_success);
    uint16_t act2 = bt.addAction(act_failure); // не має виконуватись
    bt.addChild(sel, act1);
    bt.addChild(sel, act2);
    bt.setRoot(sel);

    GsFixture f;
    EXPECT_EQ(bt.tick(f.gs, now_ms()), BTStatus::Success);
}

// ── Тест 2: Selector повертає Failure якщо всі дочірні Failure ────────────────
TEST(BehaviorTreeTest, SelectorFailureWhenAllFail) {
    BehaviorTree bt;
    uint16_t sel  = bt.addSelector();
    bt.addChild(sel, bt.addAction(act_failure));
    bt.addChild(sel, bt.addAction(act_failure));
    bt.setRoot(sel);

    GsFixture f;
    EXPECT_EQ(bt.tick(f.gs, now_ms()), BTStatus::Failure);
}

// ── Тест 3: Sequence перериває при першому Failure ────────────────────────────
TEST(BehaviorTreeTest, SequenceAbortsOnFirstFailure) {
    BehaviorTree bt;
    uint16_t seq = bt.addSequence();
    bt.addChild(seq, bt.addAction(act_success));
    bt.addChild(seq, bt.addAction(act_failure));
    bt.addChild(seq, bt.addAction(act_success)); // не має виконуватись
    bt.setRoot(seq);

    GsFixture f;
    EXPECT_EQ(bt.tick(f.gs, now_ms()), BTStatus::Failure);
}

// ── Тест 4: Wait — Running поки час не спливе, потім Success ─────────────────
TEST(BehaviorTreeTest, WaitNodeRunningThenSuccess) {
    BehaviorTree bt;
    bt.setRoot(bt.addWait(200)); // чекати 200ms

    GsFixture f;
    uint32_t t0 = now_ms();

    // Перший тік: timer ще не спливло → Running
    EXPECT_EQ(bt.tick(f.gs, t0), BTStatus::Running);

    // Тік через 300ms: timer спливло → Success
    EXPECT_EQ(bt.tick(f.gs, t0 + 300), BTStatus::Success);
}

// ── Тест 5: Repeat рахує ітерації ─────────────────────────────────────────────
// Stackless VM: якщо дочірній синхронний (не повертає Running),
// Repeat виконує всі N ітерацій за ОДИН тік і повертає Success.
// Щоб перевірити Running між ітераціями — дочірній має повертати Running.
static int g_running_count = 0;
static BTStatus act_running_twice(GameState&) {
    // Перші два виклики — Running (підвішуємо VM), третій — Success
    return (++g_running_count < 3) ? BTStatus::Running : BTStatus::Success;
}

TEST(BehaviorTreeTest, RepeatSyncChildCompletesInOneTick) {
    // Repeat(3) з синхронним дочірнім: всі ітерації за один тік → Success
    BehaviorTree bt;
    uint16_t rep = bt.addRepeat(3);
    bt.addChild(rep, bt.addAction(act_success));
    bt.setRoot(rep);

    GsFixture f;
    EXPECT_EQ(bt.tick(f.gs, now_ms()), BTStatus::Success);
}

TEST(BehaviorTreeTest, RepeatAsyncChildRunsMultipleTicks) {
    // Repeat(1) з дочірнім що повертає Running двічі перед Success
    g_running_count = 0;
    BehaviorTree bt;
    uint16_t rep = bt.addRepeat(1);
    bt.addChild(rep, bt.addAction(act_running_twice));
    bt.setRoot(rep);

    GsFixture f;
    uint32_t t = now_ms();

    // Тік 1: дочірній повертає Running (g_running_count=1) → Repeat чекає
    EXPECT_EQ(bt.tick(f.gs, t), BTStatus::Running);
    // Тік 2: дочірній повертає Running (g_running_count=2) → Repeat чекає
    EXPECT_EQ(bt.tick(f.gs, t), BTStatus::Running);
    // Тік 3: дочірній повертає Success (g_running_count=3), 1/1 → Success
    EXPECT_EQ(bt.tick(f.gs, t), BTStatus::Success);
}
