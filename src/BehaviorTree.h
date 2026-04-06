#pragma once
#include "game_state.h"
#include <cstdint>
#include <cstring>
#include <chrono>

// ── Статуси виконання ─────────────────────────────────────────────────────────
enum class BTStatus : uint8_t {
    Success,   // вузол завершився успішно
    Failure,   // вузол провалився
    Running    // вузол виконується (продовжити наступний тік)
};

// ── Типи вузлів ───────────────────────────────────────────────────────────────
enum class BTNodeType : uint8_t {
    Selector,   // повертає Success при першому Success; Failure якщо всі Failure
    Sequence,   // повертає Success якщо всі Success; Failure при першому Failure
    Condition,  // bool(*)(GameState&) → Success/Failure (leaf)
    Action,     // BTStatus(*)(GameState&) → Success/Failure/Running (leaf)
    Inverter,   // інвертує результат єдиного дочірнього
    Repeat,     // повторює дочірній N разів (data=0 → нескінченно до Failure)
    Wait        // чекає data мілісекунд, потім Success
};

using BTConditionFunc = bool(*)(GameState&);
using BTActionFunc    = BTStatus(*)(GameState&);

// ── BTNode: 24 bytes, cache-line friendly ─────────────────────────────────────
// Всі вузли в плоскому масиві — без heap allocations.
struct BTNode {
    BTNodeType type;        // 1 byte
    uint8_t    flags;       // 1 byte (reserved)
    uint16_t   parent;      // 2 bytes — індекс батька (INVALID якщо root)
    uint16_t   childStart;  // 2 bytes — початок в m_children[]
    uint16_t   childCount;  // 2 bytes — кількість дочірніх
    uint32_t   data;        // 4 bytes — Repeat: count; Wait: ms
    union {
        BTConditionFunc condition; // для Condition
        BTActionFunc    action;    // для Action
        void*           _padding;  // вирівнювання до 8 bytes
    };
};
static_assert(sizeof(BTNode) == 24, "BTNode must be 24 bytes");

// ── BTState: 12 bytes на вузол (runtime стан) ─────────────────────────────────
struct BTState {
    uint16_t currentChild; // Selector/Sequence: індекс активного дочірнього
    uint16_t counter;      // Repeat: лічильник ітерацій
    uint32_t timer;        // Wait: nowMs + wait_ms (0 = не активний)
};
static_assert(sizeof(BTState) == 8, "BTState must be 8 bytes");

// ── BehaviorTree ──────────────────────────────────────────────────────────────
// Stackless VM — жодної рекурсії, жодного heap, жодного virtual call.
// Весь стан в плоских масивах: ~9KB для 256 вузлів.
//
// Build API: addSelector/Sequence/Condition/Action/Inverter/Repeat/Wait
//            addChild(parent, child), setRoot(node)
// Runtime:   tick(gs, nowMs) — один тік, повертає BTStatus
// Reset:     reset() — скидає runtime стан (не структуру дерева)
class BehaviorTree {
public:
    static constexpr uint16_t INVALID      = 0xFFFF;
    static constexpr uint16_t MAX_NODES    = 256;
    static constexpr uint16_t MAX_CHILDREN = 512;

    BehaviorTree();

    // ── Build API ─────────────────────────────────────────────────────────────
    uint16_t addSelector ();
    uint16_t addSequence ();
    uint16_t addInverter ();
    uint16_t addRepeat   (uint32_t count);  // 0 = нескінченно (до Failure)
    uint16_t addWait     (uint32_t ms);
    uint16_t addCondition(BTConditionFunc func);
    uint16_t addAction   (BTActionFunc    func);

    void addChild(uint16_t parent, uint16_t child);
    void setRoot (uint16_t node);

    bool isValid() const { return m_root != INVALID && m_nodeCount > 0; }

    // ── Runtime ───────────────────────────────────────────────────────────────
    BTStatus tick(GameState& gs, uint32_t nowMs);
    void     reset();  // скидає BTState[] (не BTNode[])

    // ── Metrics ───────────────────────────────────────────────────────────────
    uint16_t nodeCount()     const { return m_nodeCount; }
    uint32_t tickCount()     const { return m_tickCount; }
    uint64_t totalTimeUs()   const { return m_totalTimeUs; }
    uint32_t avgTimeUs()     const {
        return m_tickCount > 0 ? (uint32_t)(m_totalTimeUs / m_tickCount) : 0;
    }

private:
    BTNode   m_nodes   [MAX_NODES];
    uint16_t m_children[MAX_CHILDREN];
    BTState  m_state   [MAX_NODES];

    uint16_t m_nodeCount  = 0;
    uint16_t m_childCount = 0;
    uint16_t m_root       = INVALID;

    uint32_t m_tickCount   = 0;
    uint64_t m_totalTimeUs = 0;
};
