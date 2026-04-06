#include "BehaviorTree.h"

BehaviorTree::BehaviorTree() {
    memset(m_nodes,    0, sizeof(m_nodes));
    memset(m_children, 0, sizeof(m_children));
    memset(m_state,    0, sizeof(m_state));
}

// ── Build helpers ─────────────────────────────────────────────────────────────

// childStart is set lazily in addChild() — do NOT pass m_childCount here
static inline void initNode(BTNode& n, BTNodeType t) {
    n.type       = t;
    n.flags      = 0;
    n.parent     = BehaviorTree::INVALID;
    n.childStart = 0;      // overwritten on first addChild call
    n.childCount = 0;
    n.data       = 0;
    n._padding   = nullptr;
}

uint16_t BehaviorTree::addSelector() {
    uint16_t i = m_nodeCount++;
    initNode(m_nodes[i], BTNodeType::Selector);
    return i;
}
uint16_t BehaviorTree::addSequence() {
    uint16_t i = m_nodeCount++;
    initNode(m_nodes[i], BTNodeType::Sequence);
    return i;
}
uint16_t BehaviorTree::addInverter() {
    uint16_t i = m_nodeCount++;
    initNode(m_nodes[i], BTNodeType::Inverter);
    return i;
}
uint16_t BehaviorTree::addRepeat(uint32_t count) {
    uint16_t i = m_nodeCount++;
    initNode(m_nodes[i], BTNodeType::Repeat);
    m_nodes[i].data = count;
    return i;
}
uint16_t BehaviorTree::addWait(uint32_t ms) {
    uint16_t i = m_nodeCount++;
    initNode(m_nodes[i], BTNodeType::Wait);
    m_nodes[i].data = ms;
    return i;
}
uint16_t BehaviorTree::addCondition(BTConditionFunc func) {
    uint16_t i = m_nodeCount++;
    initNode(m_nodes[i], BTNodeType::Condition);
    m_nodes[i].condition = func;
    return i;
}
uint16_t BehaviorTree::addAction(BTActionFunc func) {
    uint16_t i = m_nodeCount++;
    initNode(m_nodes[i], BTNodeType::Action);
    m_nodes[i].action = func;
    return i;
}

void BehaviorTree::addChild(uint16_t parent, uint16_t child) {
    BTNode& pn = m_nodes[parent];
    if (pn.childCount == 0)
        pn.childStart = m_childCount;   // first child fixes childStart
    m_children[m_childCount++] = child;
    pn.childCount++;
    m_nodes[child].parent = parent;
}

void BehaviorTree::setRoot(uint16_t node) { m_root = node; }

void BehaviorTree::reset() {
    memset(m_state, 0, sizeof(BTState) * m_nodeCount);
}

// ── Stackless VM ──────────────────────────────────────────────────────────────
// Program counter (pc) = індекс поточного вузла.
// result = статус що повернув попередній дочірній вузол.
// Початок: pc=root, result=Running (означає "перший вхід").
//
// Правило виходу: pc==INVALID (повернулись вище root) або Action повернув Running.

BTStatus BehaviorTree::tick(GameState& gs, uint32_t nowMs) {
    if (!isValid()) return BTStatus::Failure;

    auto t0 = std::chrono::high_resolution_clock::now();

    uint16_t pc     = m_root;
    BTStatus result = BTStatus::Running; // Running = "перший вхід у вузол"

    while (true) {

        if (pc == INVALID) break; // вийшли вище root

        BTNode&  nd = m_nodes[pc];
        BTState& st = m_state[pc];

        switch (nd.type) {

        // ── Selector: перший Success → Success; всі Failure → Failure ────────
        case BTNodeType::Selector:
            if (result == BTStatus::Success) {
                // Дочірній успішний → Selector успіх, повертаємось до батька
                st.currentChild = 0;
                pc = nd.parent; result = BTStatus::Success; continue;
            }
            if (result == BTStatus::Failure) {
                st.currentChild++; // наступний дочірній
            }
            // Running = перший вхід або продовжуємо
            if (st.currentChild >= nd.childCount) {
                st.currentChild = 0;
                pc = nd.parent; result = BTStatus::Failure; continue;
            }
            pc = m_children[nd.childStart + st.currentChild];
            result = BTStatus::Running; continue;

        // ── Sequence: всі Success → Success; перший Failure → Failure ────────
        case BTNodeType::Sequence:
            if (result == BTStatus::Failure) {
                st.currentChild = 0;
                pc = nd.parent; result = BTStatus::Failure; continue;
            }
            if (result == BTStatus::Success) {
                st.currentChild++;
            }
            if (st.currentChild >= nd.childCount) {
                st.currentChild = 0;
                pc = nd.parent; result = BTStatus::Success; continue;
            }
            pc = m_children[nd.childStart + st.currentChild];
            result = BTStatus::Running; continue;

        // ── Condition: виконати bool функцію ──────────────────────────────────
        case BTNodeType::Condition:
            result = nd.condition(gs) ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;

        // ── Action: виконати BTStatus функцію ────────────────────────────────
        case BTNodeType::Action:
            result = nd.action(gs);
            if (result == BTStatus::Running) goto exit_loop; // залишаємось тут
            pc = nd.parent; continue;

        // ── Inverter: інвертувати результат єдиного дочірнього ───────────────
        case BTNodeType::Inverter:
            if (result == BTStatus::Running) {
                // Перший вхід: передаємо управління дочірньому
                if (nd.childCount > 0) {
                    pc = m_children[nd.childStart];
                    result = BTStatus::Running; continue;
                }
                pc = nd.parent; result = BTStatus::Failure; continue;
            }
            // Інвертуємо
            result = (result == BTStatus::Success)
                     ? BTStatus::Failure : BTStatus::Success;
            pc = nd.parent; continue;

        // ── Repeat: повторювати дочірній N разів ─────────────────────────────
        case BTNodeType::Repeat:
            if (result == BTStatus::Running) {
                // Перший вхід: запускаємо дочірній
                if (nd.childCount > 0) {
                    pc = m_children[nd.childStart];
                    result = BTStatus::Running; continue;
                }
                pc = nd.parent; result = BTStatus::Success; continue;
            }
            if (result == BTStatus::Failure) {
                // Failure від дочірнього → зупиняємось
                st.counter = 0;
                pc = nd.parent; result = BTStatus::Failure; continue;
            }
            // Success від дочірнього
            st.counter++;
            if (nd.data > 0 && st.counter >= nd.data) {
                // Досягли ліміту
                st.counter = 0;
                pc = nd.parent; result = BTStatus::Success; continue;
            }
            // Повторюємо: скидаємо стан дочірнього і запускаємо знову
            if (nd.childCount > 0) {
                uint16_t ci = m_children[nd.childStart];
                m_state[ci] = BTState{};
                pc = ci; result = BTStatus::Running; continue;
            }
            pc = nd.parent; result = BTStatus::Success; continue;

        // ── Wait: чекати N мілісекунд ─────────────────────────────────────────
        case BTNodeType::Wait:
            if (st.timer == 0) {
                // Перший вхід: запам'ятовуємо час закінчення
                st.timer = nowMs + nd.data;
            }
            // Wrap-safe порівняння через signed cast
            if (static_cast<int32_t>(nowMs - st.timer) >= 0) {
                st.timer = 0;
                pc = nd.parent; result = BTStatus::Success; continue;
            }
            result = BTStatus::Running;
            goto exit_loop;

        default:
            goto exit_loop;
        }
    }

exit_loop:
    {
        auto t1 = std::chrono::high_resolution_clock::now();
        m_totalTimeUs += std::chrono::duration_cast<
            std::chrono::microseconds>(t1 - t0).count();
        m_tickCount++;
    }
    return result;
}
