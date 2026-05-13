#pragma once
#include "Blackboard.h"
#include "BotMood.h"
#include <chrono>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

struct GameState;

// ── StrategicDirective ────────────────────────────────────────────────────────
// Analogue of CATHODE Director "hints" published to Blackboard.
// Agent (BotBehaviorTree) reads BB::Int::CURRENT_DIRECTIVE each tick
// and adjusts branch priorities accordingly.
enum class StrategicDirective : uint8_t {
    FARM_HERE      = 0,  // current zone is good, keep farming
    REPOSITION     = 1,  // low KPM or death streak → move to new spot
    REST_FIRST     = 2,  // cautious mood + low HP → rest before engaging
    RETURN_TO_ZONE = 3,  // strayed > 2× zone radius → return
    FLEE           = 4,  // FLEE mood → escape priority
};

constexpr std::string_view directiveName(StrategicDirective d) noexcept {
    switch (d) {
    case StrategicDirective::FARM_HERE:      return "FARM_HERE";
    case StrategicDirective::REPOSITION:     return "REPOSITION";
    case StrategicDirective::REST_FIRST:     return "REST_FIRST";
    case StrategicDirective::RETURN_TO_ZONE: return "RETURN_TO_ZONE";
    case StrategicDirective::FLEE:           return "FLEE";
    }
    return "?";
}

// ── ZoneRecord ────────────────────────────────────────────────────────────────
// Rolling statistics for the current farm zone.
// Inspired by CATHODE's area-sweep history used by the Director.
struct ZoneRecord {
    float cx     = 0.f;
    float cy     = 0.f;
    float radius = 300.f;
    float kpm    = 0.f;    // kills per minute (rolling 60s)
    float death_rate = 0.f; // deaths per hour
    int   samples    = 0;

    // score = kpm / (1 + death_rate) * confidence
    // confidence rises linearly with samples up to 10
    float score() const noexcept {
        float conf = samples < 10 ? static_cast<float>(samples) / 10.f : 1.f;
        return kpm / (1.f + death_rate) * conf;
    }
};

// ── DirectorSystem ────────────────────────────────────────────────────────────
// Strategic AI layer.  Called from Brain::Process() every tick.
// Runs a full analysis cycle every ANALYSIS_MS (500ms), writing:
//   BB::Int::CURRENT_MOOD       — from MoodManager
//   BB::Int::CURRENT_DIRECTIVE  — from publishDirective()
//   BB::Float::KILLS_PER_MIN    — rolling 60s kill rate
//   BB::Float::ZONE_CX/CY/RADIUS — initialised once from first valid coords
//   BB::Bool::ZONE_VALID        — set when zone has been initialised
//   BB::Bool::FLEE_ACTIVE       — set when directive == FLEE
//
// Thread model: SINGLE-THREADED (called from Brain's main thread).
// Analysis takes <1ms → negligible in 100ms tick budget.
class DirectorSystem {
public:
    using LogFn = std::function<void(const std::string&)>;
    using Clock = std::chrono::steady_clock;
    using TP    = Clock::time_point;

    explicit DirectorSystem(Blackboard& bb);

    // Called from Brain::Process() each tick.
    // Runs analyse() only when ANALYSIS_MS has elapsed.
    void tick(const GameState& gs);

    // Full analysis cycle (public for testing without timing guard).
    void analyse(const GameState& gs);

    void setLogFn(LogFn fn) { m_log = std::move(fn); }

    StrategicDirective currentDirective() const noexcept {
        return static_cast<StrategicDirective>(
            m_bb.getI(BB::Int::CURRENT_DIRECTIVE));
    }
    BotMood currentMood() const noexcept {
        return static_cast<BotMood>(m_bb.getI(BB::Int::CURRENT_MOOD));
    }
    ZoneRecord currentZone() const noexcept { return m_zone; }
    float killsPerMin()      const noexcept {
        return m_bb.getF(BB::Float::KILLS_PER_MIN);
    }

private:
    void updateKillRate  (const GameState& gs);
    void updateDeathStreak(const GameState& gs);
    void updateZone      (const GameState& gs);
    void updateMood      (const GameState& gs);
    void publishDirective(const GameState& gs);

    void log(const std::string& msg);

    Blackboard& m_bb;
    MoodManager m_mood_mgr;
    LogFn       m_log;

    TP m_last_analysis{};
    static constexpr int ANALYSIS_MS = 500;

    // Rolling kill window (last 60s)
    struct KillEvent { TP time; };
    std::vector<KillEvent> m_kill_window;
    static constexpr int   KILL_WINDOW_SECS = 60;

    // Zone state
    ZoneRecord m_zone{};
    bool       m_zone_initialized = false;

    // Death tracking (session-level)
    int  m_consecutive_deaths = 0;
    bool m_prev_is_dead       = false;

    // Delta detection vs previous analysis
    double m_prev_secs_since_kill = 9999.0;
};
