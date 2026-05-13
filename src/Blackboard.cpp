#include "Blackboard.h"
#include "game_state.h"
#include <algorithm>
#include <iomanip>
#include <sstream>

Blackboard::Blackboard() {
    for (auto& a : m_floats) a.store(0.f, std::memory_order_relaxed);
    for (auto& a : m_ints  ) a.store(0,   std::memory_order_relaxed);
    m_bools.store(0, std::memory_order_relaxed);
}

void Blackboard::syncFromGameState(const GameState& gs) {
    // Per-tick agent snapshot — does NOT touch Director-owned slots:
    // ZONE_*, KILLS_PER_MIN, MOOD_INTENSITY, SECS_SINCE_KILL, CURRENT_MOOD,
    // CURRENT_DIRECTIVE, DEAD_STREAK, FLEE_ACTIVE, ZONE_VALID.

    setF(BB::Float::PLAYER_HP_PCT,  gs.hp  / 100.f);
    setF(BB::Float::PLAYER_MP_PCT,  gs.mp  / 100.f);
    setF(BB::Float::PLAYER_CP_PCT,  gs.cp  / 100.f);
    setF(BB::Float::TARGET_HP_PCT,  gs.has_target ? gs.target_hp / 100.f : 0.f);
    setF(BB::Float::MOB_DENSITY,
         std::min(static_cast<float>(gs.kl_alive_count) / 10.f, 1.f));

    setI(BB::Int::ALIVE_MOB_COUNT, gs.kl_alive_count);

    setB(BB::Bool::HAVE_TARGET,  gs.has_target);
    setB(BB::Bool::IS_DEAD,      gs.is_dead);
    setB(BB::Bool::IN_GRACE,     gs.in_grace);
    setB(BB::Bool::CLOSE_THREAT, gs.minimap_close_threat);
    setB(BB::Bool::COORDS_VALID, gs.coords_valid);
}

std::string Blackboard::dump() const {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2)
        << "BB{"
        << "hp="   << getF(BB::Float::PLAYER_HP_PCT)
        << " mp="  << getF(BB::Float::PLAYER_MP_PCT)
        << " kpm=" << getF(BB::Float::KILLS_PER_MIN)
        << " mood=" << getI(BB::Int::CURRENT_MOOD)
        << " dir="  << getI(BB::Int::CURRENT_DIRECTIVE)
        << " deaths=" << getI(BB::Int::DEAD_STREAK)
        << " tgt="  << (getB(BB::Bool::HAVE_TARGET)  ? "1" : "0")
        << " flee=" << (getB(BB::Bool::FLEE_ACTIVE)   ? "1" : "0")
        << " zone=" << (getB(BB::Bool::ZONE_VALID)    ? "1" : "0")
        << "}";
    return oss.str();
}
