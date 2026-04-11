#pragma once
#include "game_state.h"
#include "Stats.h"

// RewardCalculator: явна reward function для RL.
// Виклик один раз за тік в BotBehaviorTree::tick() ПІСЛЯ виконання дії.
namespace RewardCalculator {

struct RewardSignals {
    bool kill_happened      = false;
    bool death_happened     = false;
    bool targeting_failed   = false;
    bool buff_done          = false;
};

static constexpr float REWARD_KILL           =  1.0f;
static constexpr float REWARD_DEATH          = -5.0f;
static constexpr float REWARD_TARGETING_FAIL = -0.01f;
static constexpr float REWARD_BUFF_DONE      =  0.1f;
static constexpr float REWARD_IDLE_PENALTY   = -0.001f;

inline float compute(const RewardSignals& sig, Stats& stats) {
    float r = REWARD_IDLE_PENALTY;
    if (sig.kill_happened)    r += REWARD_KILL;
    if (sig.death_happened)   r += REWARD_DEATH;
    if (sig.targeting_failed) r += REWARD_TARGETING_FAIL;
    if (sig.buff_done)        r += REWARD_BUFF_DONE;
    stats.last_reward        = r;
    stats.cumulative_reward += r;
    return r;
}

} // namespace RewardCalculator
