#pragma once
#include <cstdint>
#include <cmath>
#include <string>

// ── Типи об'єктів KnownList ──────────────────────────────────────────────────
enum class L2ObjectType : int {
    Mob     = 0,
    Player  = 1,
    Item    = 2,
    Static  = 3,
    Unknown = -1,
};

// ── Базовий об'єкт KnownList ─────────────────────────────────────────────────
struct L2Object {
    uintptr_t   memPtr   = 0;                    // адреса в пам'яті процесу
    int         objectID = 0;
    std::string name;                            // назва (моба, гравця) — порожньо якщо не прочитано
    L2ObjectType type    = L2ObjectType::Unknown;
    float x = 0.f, y = 0.f, z = 0.f;            // world coordinates (L2 units)

    float distanceTo(float px, float py) const {
        float dx = x - px, dy = y - py;
        return std::sqrt(dx * dx + dy * dy);
    }
    bool valid() const { return memPtr != 0; }
};

// ── Персонаж (моб або гравець) ───────────────────────────────────────────────
struct L2Character : L2Object {
    float hp    = 0.f;
    float hpMax = 0.f;
    bool  isDead = false;
    int   level  = 0;     // рівень моба (0 якщо не відкалібровано)
    float mp     = 0.f;   // поточний MP
    float mpMax  = 0.f;   // максимальний MP

    float hpPercent() const {
        return hpMax > 0.f ? (hp / hpMax * 100.f) : 0.f;
    }
    float mpPercent() const {
        return mpMax > 0.f ? (mp / mpMax * 100.f) : 0.f;
    }
};
