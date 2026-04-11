#pragma once
#include <string>
#include <chrono>

// Статистика фарм-сесії.
struct Stats {
    int kills = 0;
    int deaths = 0;
    int attacks = 0;
    int hp_potions = 0;
    int mp_potions = 0;
    int targeting_failures = 0;

    std::chrono::steady_clock::time_point session_start = std::chrono::steady_clock::now();

    void RecordKill()              { kills++; }
    void RecordDeath()             { deaths++; }
    void RecordAttack()            { attacks++; }
    void RecordHPPotion()          { hp_potions++; }
    void RecordMPPotion()          { mp_potions++; }
    void RecordTargetingFailure()  { targeting_failures++; }

    // RL reward tracking
    float last_reward       = 0.f;
    float cumulative_reward = 0.f;
    int   episode_kills     = 0;
    int   episode_steps     = 0;

    void resetEpisode()       { episode_kills = 0; episode_steps = 0; }
    void recordEpisodeKill()  { episode_kills++; kills++; }
    void recordEpisodeStep()  { episode_steps++; }

    int UptimeSec() const;
    std::string UptimeStr() const;
    void PrintSummary() const;
    void SaveToFile() const; // append JSON to logs/stats_YYYY-MM-DD.json
};
