#pragma once
#include <atomic>
#include <cstdint>
#include <string>

// ── Blackboard ────────────────────────────────────────────────────────────────
// Inspired by CATHODE CHARACTER_BB_ENTRY_TYPE.
// Lock-free shared state bridging Director (strategic, 500ms cadence)
// and Agent/BT (tactical, every tick ~100ms).
//
// Three typed flat arrays (float / int / bool) backed by std::atomic.
// Director writes with memory_order_release; BT reads with relaxed
// (same-thread fast path) — correct on x86 ABI.
// Bool slots are bit-packed into a single atomic<uint64_t>.

namespace BB {

// Float slots ([0,1] normalized where noted, raw units otherwise)
enum Float : uint8_t {
    PLAYER_HP_PCT    =  0,  // [0,1]
    PLAYER_MP_PCT    =  1,  // [0,1]
    PLAYER_CP_PCT    =  2,  // [0,1]
    TARGET_HP_PCT    =  3,  // [0,1]
    TARGET_DIST_PX   =  4,  // minimap pixels (raw)
    KILLS_PER_MIN    =  5,  // rolling 60s window
    MOB_DENSITY      =  6,  // [0,1] alive_mobs/10 normalised
    ZONE_CX          =  7,  // Director: farm zone centre X (world)
    ZONE_CY          =  8,  // Director: farm zone centre Y (world)
    ZONE_RADIUS      =  9,  // Director: farm zone radius
    MOOD_INTENSITY   = 10,  // [0,1] continuous mood strength
    SECS_SINCE_KILL  = 11,  // seconds since last kill
    FLOAT_COUNT      = 12,
};

// Int slots
enum Int : uint8_t {
    ALIVE_MOB_COUNT    = 0,  // KL alive mobs near player
    UNREACHABLE_STREAK = 1,  // consecutive unreachable without kill (BT-owned)
    DEAD_STREAK        = 2,  // consecutive deaths this session (Director-owned)
    COMBAT_STATE       = 3,  // BotCombatState enum value (reserved)
    CURRENT_MOOD       = 4,  // BotMood enum value (Director writes)
    CURRENT_DIRECTIVE  = 5,  // StrategicDirective enum value (Director writes)
    INT_COUNT          = 6,
};

// Bool slots (bit index into atomic<uint64_t>)
enum Bool : uint8_t {
    HAVE_TARGET      = 0,
    IS_DEAD          = 1,
    IN_GRACE         = 2,
    CLOSE_THREAT     = 3,  // minimap dot < 70px
    COORDS_VALID     = 4,  // MemReader coords valid
    TARGET_REACHABLE = 5,  // last navigation attempt succeeded
    LOOT_PENDING     = 6,
    BUFF_PENDING     = 7,
    FLEE_ACTIVE      = 8,  // Director: FLEE directive active
    ZONE_VALID       = 9,  // Director zone coords are usable
    BOOL_COUNT       = 10,
};

static_assert(FLOAT_COUNT <= 32);
static_assert(INT_COUNT   <= 16);
static_assert(BOOL_COUNT  <= 64);

} // namespace BB

struct GameState; // for syncFromGameState

class Blackboard {
public:
    Blackboard();

    // ── Float ─────────────────────────────────────────────────────────────────
    float getF(BB::Float k) const noexcept {
        return m_floats[k].load(std::memory_order_relaxed);
    }
    void setF(BB::Float k, float v) noexcept {
        m_floats[k].store(v, std::memory_order_release);
    }

    // ── Int ───────────────────────────────────────────────────────────────────
    int  getI(BB::Int k) const noexcept {
        return m_ints[k].load(std::memory_order_relaxed);
    }
    void setI(BB::Int k, int v) noexcept {
        m_ints[k].store(v, std::memory_order_release);
    }

    // ── Bool (bit-packed) ─────────────────────────────────────────────────────
    bool getB(BB::Bool k) const noexcept {
        return (m_bools.load(std::memory_order_relaxed) >> k) & 1ull;
    }
    void setB(BB::Bool k, bool v) noexcept {
        const uint64_t mask = uint64_t(1) << k;
        if (v) m_bools.fetch_or (mask, std::memory_order_release);
        else   m_bools.fetch_and(~mask, std::memory_order_release);
    }

    // Sync per-tick GameState fields into BB (called by Brain::updateGameState).
    // Does NOT overwrite Director-owned fields (ZONE_*, MOOD_*, DIRECTIVE).
    void syncFromGameState(const GameState& gs);

    std::string dump() const;

private:
    std::atomic<float>    m_floats[BB::FLOAT_COUNT]{};
    std::atomic<int>      m_ints  [BB::INT_COUNT  ]{};
    std::atomic<uint64_t> m_bools {0};
};
