#include "DirectorSystem.h"
#include "game_state.h"
#include <algorithm>
#include <cmath>

DirectorSystem::DirectorSystem(Blackboard& bb) : m_bb(bb) {
    m_last_analysis = Clock::now();
    // Publish safe defaults immediately
    m_bb.setI(BB::Int::CURRENT_MOOD,      static_cast<int>(BotMood::NEUTRAL));
    m_bb.setI(BB::Int::CURRENT_DIRECTIVE, static_cast<int>(StrategicDirective::FARM_HERE));
    m_bb.setF(BB::Float::KILLS_PER_MIN,   0.f);
    m_bb.setB(BB::Bool::FLEE_ACTIVE,      false);
    m_bb.setB(BB::Bool::ZONE_VALID,       false);
}

void DirectorSystem::tick(const GameState& gs) {
    auto now = Clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          now - m_last_analysis).count();
    if (elapsed_ms < ANALYSIS_MS) return;
    m_last_analysis = now;
    analyse(gs);
}

void DirectorSystem::analyse(const GameState& gs) {
    updateKillRate(gs);
    updateDeathStreak(gs);
    updateZone(gs);
    updateMood(gs);
    publishDirective(gs);
}

// ── Kill rate ─────────────────────────────────────────────────────────────────

void DirectorSystem::updateKillRate(const GameState& gs) {
    // Kill detected: secs_since_last_kill decreased by ≥0.3s since last analysis.
    // The BT already flags kl_mob_died but that's per-tick; here we use the
    // secs_since_last_kill timer which resets on each kill.
    if (gs.secs_since_last_kill < m_prev_secs_since_kill - 0.3) {
        m_kill_window.push_back({Clock::now()});
        log("[Dir] kill detected  window=" + std::to_string(m_kill_window.size()));
    }
    m_prev_secs_since_kill = gs.secs_since_last_kill;

    // Prune events older than the window
    auto cutoff = Clock::now() - std::chrono::seconds(KILL_WINDOW_SECS);
    m_kill_window.erase(
        std::remove_if(m_kill_window.begin(), m_kill_window.end(),
                       [&](const KillEvent& e) { return e.time < cutoff; }),
        m_kill_window.end());

    float kpm = static_cast<float>(m_kill_window.size())
              / (static_cast<float>(KILL_WINDOW_SECS) / 60.f);
    m_bb.setF(BB::Float::KILLS_PER_MIN,  kpm);
    m_bb.setF(BB::Float::SECS_SINCE_KILL,
              static_cast<float>(gs.secs_since_last_kill));
}

// ── Death streak ──────────────────────────────────────────────────────────────

void DirectorSystem::updateDeathStreak(const GameState& gs) {
    // Rising edge on is_dead → new death
    if (!m_prev_is_dead && gs.is_dead) {
        ++m_consecutive_deaths;
        log("[Dir] death #" + std::to_string(m_consecutive_deaths));
    }
    // Falling edge: respawned and active (not in grace) → reset streak on kill
    if (m_prev_is_dead && !gs.is_dead && !gs.in_grace) {
        // Keep streak: it resets when KPM recovers (checked in publishDirective)
    }
    m_prev_is_dead = gs.is_dead;
    m_bb.setI(BB::Int::DEAD_STREAK, m_consecutive_deaths);
}

// ── Zone ──────────────────────────────────────────────────────────────────────

void DirectorSystem::updateZone(const GameState& gs) {
    if (!gs.coords_valid) return;

    if (!m_zone_initialized) {
        m_zone.cx     = gs.player_x;
        m_zone.cy     = gs.player_y;
        m_zone.radius = 300.f;
        m_zone_initialized = true;

        m_bb.setF(BB::Float::ZONE_CX,     m_zone.cx);
        m_bb.setF(BB::Float::ZONE_CY,     m_zone.cy);
        m_bb.setF(BB::Float::ZONE_RADIUS, m_zone.radius);
        m_bb.setB(BB::Bool::ZONE_VALID,   true);

        log("[Dir] zone init  cx=" + std::to_string(static_cast<int>(m_zone.cx))
            + " cy=" + std::to_string(static_cast<int>(m_zone.cy)));
        return;
    }

    m_zone.kpm    = m_bb.getF(BB::Float::KILLS_PER_MIN);
    m_zone.samples = std::min(m_zone.samples + 1, 100);

    // Log poor zone performance (doesn't change zone automatically — Director
    // only suggests REPOSITION, Agent decides when to act on it)
    if (m_consecutive_deaths >= 3 && m_zone.kpm < 1.0f) {
        log("[Dir] zone score low  kpm=" + std::to_string(m_zone.kpm)
            + " deaths=" + std::to_string(m_consecutive_deaths));
    }
}

// ── Mood ──────────────────────────────────────────────────────────────────────

void DirectorSystem::updateMood(const GameState& gs) {
    float hp_pct  = gs.hp_valid ? gs.hp / 100.f : 1.f;
    float kpm     = m_bb.getF(BB::Float::KILLS_PER_MIN);
    float no_kill = static_cast<float>(gs.secs_since_last_kill);

    MoodState ms = m_mood_mgr.evaluate(hp_pct, kpm, m_consecutive_deaths,
                                       gs.kl_alive_count, no_kill);
    m_bb.setI(BB::Int::CURRENT_MOOD,     static_cast<int>(ms.mood));
    m_bb.setF(BB::Float::MOOD_INTENSITY, ms.value);

    static BotMood s_prev = BotMood::NEUTRAL;
    if (ms.mood != s_prev) {
        log(std::string("[Dir] mood → ") + std::string(moodName(ms.mood)));
        s_prev = ms.mood;
    }
}

// ── Directive ─────────────────────────────────────────────────────────────────

void DirectorSystem::publishDirective(const GameState& gs) {
    BotMood mood = currentMood();
    StrategicDirective dir = StrategicDirective::FARM_HERE;

    if (mood == BotMood::FLEE) {
        dir = StrategicDirective::FLEE;

    } else if (mood == BotMood::CAUTIOUS && gs.hp < 40) {
        dir = StrategicDirective::REST_FIRST;

    } else if (mood == BotMood::SUSPICIOUS) {
        dir = StrategicDirective::REPOSITION;
        // Once we reposition, reset death streak so we don't loop
        m_consecutive_deaths = 0;

    } else if (m_zone_initialized && gs.coords_valid) {
        float dx   = gs.player_x - m_zone.cx;
        float dy   = gs.player_y - m_zone.cy;
        float dist = std::sqrt(dx * dx + dy * dy);
        if (dist > m_zone.radius * 2.0f)
            dir = StrategicDirective::RETURN_TO_ZONE;
    }

    m_bb.setI(BB::Int::CURRENT_DIRECTIVE, static_cast<int>(dir));
    m_bb.setB(BB::Bool::FLEE_ACTIVE,      dir == StrategicDirective::FLEE);

    static StrategicDirective s_prev = StrategicDirective::FARM_HERE;
    if (dir != s_prev) {
        log(std::string("[Dir] directive → ") + std::string(directiveName(dir)));
        s_prev = dir;
    }
}

// ── Logging ───────────────────────────────────────────────────────────────────

void DirectorSystem::log(const std::string& msg) {
    if (m_log) m_log(msg);
}
