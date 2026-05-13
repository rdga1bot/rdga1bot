#pragma once
#include <cstdint>
#include "game_state.h"

// ── SenseSystem ───────────────────────────────────────────────────────────────
// Inspired by CATHODE SENSE_SET / SENSORY_TYPE / VIEWCONE_TYPE.
//
// Maps available perception sources to a tiered quality level.
// Higher tier → more accurate decisions.
//
// Bot senses (always available to always-available):
//   MINIMAL    — OCR HP/MP/CP bars (screen reading)
//   STANDARD   — + minimap dots (vision worker, always active)
//   HEIGHTENED — + player XYZ from process memory (MemReader valid)
//   FULL       — + KnownList mob positions/HP (WorldState + MemReader)

enum class SenseSet : uint8_t {
    MINIMAL    = 0,
    STANDARD   = 1,
    HEIGHTENED = 2,
    FULL       = 3,
};

// Specific sensory signals (analogue of CATHODE SENSORY_TYPE)
enum class SensoryType : uint8_t {
    OCR_HP_BAR     = 0,  // HP/MP/CP from screen
    MINIMAP_THREAT = 1,  // close enemy dot on minimap
    MEMORY_COORDS  = 2,  // player XYZ from process memory
    KL_MOB_COUNT   = 3,  // alive mob count from KnownList
    TARGET_HP      = 4,  // target HP from OCR
    KL_MOB_HP      = 5,  // mob HP from KnownList memory
};

// Returns highest tier where ALL required signals are valid.
inline SenseSet evaluateSenseSet(const GameState& gs) noexcept {
    if (!gs.hp_valid) return SenseSet::MINIMAL;

    bool has_minimap = gs.minimap_close_threat
                    || !gs.minimap_dots.empty()
                    || gs.has_target;
    if (!has_minimap)  return SenseSet::MINIMAL;

    if (!gs.coords_valid) return SenseSet::STANDARD;

    bool has_kl = !gs.kl_mobs.empty() || gs.kl_alive_count > 0;
    if (!has_kl) return SenseSet::HEIGHTENED;

    return SenseSet::FULL;
}

// Check if a specific sensory signal is active.
inline bool hasSense(const GameState& gs, SensoryType s) noexcept {
    switch (s) {
    case SensoryType::OCR_HP_BAR:     return gs.hp_valid;
    case SensoryType::MINIMAP_THREAT: return gs.minimap_close_threat;
    case SensoryType::MEMORY_COORDS:  return gs.coords_valid;
    case SensoryType::KL_MOB_COUNT:   return gs.kl_alive_count > 0;
    case SensoryType::TARGET_HP:      return gs.has_target && gs.target_hp > 0;
    case SensoryType::KL_MOB_HP:      return !gs.kl_mobs.empty();
    }
    return false;
}
