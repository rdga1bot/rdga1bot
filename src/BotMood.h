#pragma once
#include <cstdint>
#include <string_view>
#include <algorithm>

// ── BotMood ───────────────────────────────────────────────────────────────────
// Inspired by CATHODE BEHAVIOUR_MOOD_SET (NEUTRAL / AGGRESSIVE / PANICKED /
// THREAT_ESCALATION / SUSPICIOUS).
//
// Mood modulates two subsystems:
//   1. BT conditions — condNeedsRest threshold, target selection strategy
//   2. Q-learning reward shaping — kill/death scale coefficients
//
// DirectorSystem::updateMood() writes BB::Int::CURRENT_MOOD each analysis tick.
// BT nodes read via: gs.blackboard->getI(BB::Int::CURRENT_MOOD)

enum class BotMood : uint8_t {
    NEUTRAL    = 0,  // normal farming
    AGGRESSIVE = 1,  // high kill rate, dense mobs → push harder
    CAUTIOUS   = 2,  // low HP or deaths increasing → heal sooner
    FLEE       = 3,  // HP critical or surrounded → escape priority
    SUSPICIOUS = 4,  // no kills > 90s → reposition / investigate
};

enum class MoodIntensity : uint8_t {
    LOW    = 0,
    MEDIUM = 1,
    HIGH   = 2,
};

struct MoodState {
    BotMood       mood      = BotMood::NEUTRAL;
    MoodIntensity intensity = MoodIntensity::LOW;
    float         value     = 0.f;  // [0,1] continuous intensity
};

// Reward shaping multipliers applied in RewardCalculator::compute()
// when mood-aware reward is enabled (feature flag in INI).
struct MoodRewardScale {
    float kill_scale  = 1.0f;
    float death_scale = 1.0f;
    float rest_bonus  = 0.0f;  // additive on rest action (CAUTIOUS)
    float move_bonus  = 0.0f;  // additive on movement (FLEE)
};

constexpr MoodRewardScale getMoodRewardScale(BotMood m) noexcept {
    switch (m) {
    case BotMood::AGGRESSIVE: return {1.25f, 0.80f, 0.00f, 0.00f};
    case BotMood::CAUTIOUS:   return {0.80f, 1.50f, 0.20f, 0.00f};
    case BotMood::FLEE:       return {0.50f, 2.00f, 0.00f, 0.30f};
    case BotMood::SUSPICIOUS: return {1.50f, 1.00f, 0.00f, 0.00f};
    default:                  return {1.00f, 1.00f, 0.00f, 0.00f};
    }
}

constexpr std::string_view moodName(BotMood m) noexcept {
    switch (m) {
    case BotMood::NEUTRAL:    return "NEUTRAL";
    case BotMood::AGGRESSIVE: return "AGGRESSIVE";
    case BotMood::CAUTIOUS:   return "CAUTIOUS";
    case BotMood::FLEE:       return "FLEE";
    case BotMood::SUSPICIOUS: return "SUSPICIOUS";
    }
    return "?";
}

// ── MoodManager ───────────────────────────────────────────────────────────────
// Stateful evaluator. Hysteresis guard prevents rapid mood flapping:
// candidate mood must be stable for HYSTERESIS evaluations before switching,
// EXCEPT for FLEE which is always immediate (safety rule).
class MoodManager {
public:
    MoodState evaluate(float hp_pct, float kills_per_min, int dead_streak,
                       int alive_mobs, float secs_since_kill) noexcept;

    MoodState current() const noexcept { return m_state; }

    void reset() noexcept {
        m_state           = {};
        m_candidate       = BotMood::NEUTRAL;
        m_candidate_count = 0;
    }

private:
    BotMood selectCandidate(float hp_pct, float kpm, int dead_streak,
                            int alive_mobs, float secs_since_kill) const noexcept;

    MoodState m_state;
    BotMood   m_candidate       = BotMood::NEUTRAL;
    int       m_candidate_count = 0;
    static constexpr int HYSTERESIS = 3;
};

// ── Inline implementations ────────────────────────────────────────────────────

inline BotMood MoodManager::selectCandidate(float hp_pct, float kpm, int dead_streak,
                                             int alive_mobs, float secs_since_kill)
    const noexcept
{
    // FLEE: critical HP or surrounded while hurt
    if (hp_pct < 0.20f) return BotMood::FLEE;
    if (hp_pct < 0.35f && alive_mobs >= 3) return BotMood::FLEE;

    // CAUTIOUS: low HP or repeated deaths
    if (hp_pct < 0.45f) return BotMood::CAUTIOUS;
    if (dead_streak >= 2) return BotMood::CAUTIOUS;

    // SUSPICIOUS: no kills for too long
    if (secs_since_kill > 90.f) return BotMood::SUSPICIOUS;

    // AGGRESSIVE: good kill rate and healthy
    if (kpm >= 3.0f && hp_pct >= 0.60f) return BotMood::AGGRESSIVE;

    return BotMood::NEUTRAL;
}

inline MoodState MoodManager::evaluate(float hp_pct, float kpm, int dead_streak,
                                        int alive_mobs, float secs_since_kill) noexcept
{
    BotMood cand = selectCandidate(hp_pct, kpm, dead_streak, alive_mobs, secs_since_kill);

    if (cand != m_candidate) {
        m_candidate       = cand;
        m_candidate_count = 0;
    } else {
        ++m_candidate_count;
    }

    // FLEE bypasses hysteresis (safety); others need HYSTERESIS consecutive evals.
    // m_candidate_count starts at 0 on first eval → switch when count reaches HYSTERESIS-1.
    if (cand == BotMood::FLEE || m_candidate_count >= HYSTERESIS - 1)
        m_state.mood = cand;

    // Continuous intensity per mood
    float v = 0.f;
    switch (m_state.mood) {
    case BotMood::FLEE:
        v = 1.f - hp_pct;
        break;
    case BotMood::CAUTIOUS:
        v = (0.45f - std::min(hp_pct, 0.45f)) / 0.45f;
        break;
    case BotMood::AGGRESSIVE:
        v = std::min(kpm / 6.f, 1.f);
        break;
    case BotMood::SUSPICIOUS:
        v = std::min((secs_since_kill - 90.f) / 60.f, 1.f);
        break;
    default:
        v = 0.f;
        break;
    }
    v = std::max(0.f, std::min(1.f, v));

    m_state.value     = v;
    m_state.intensity = v < 0.33f ? MoodIntensity::LOW
                      : v < 0.67f ? MoodIntensity::MEDIUM
                      :             MoodIntensity::HIGH;
    return m_state;
}
