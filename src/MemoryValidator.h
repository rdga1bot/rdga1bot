#pragma once
#include "l2_objects.h"
#include "MemReader.h"
#include <string>
#include <cmath>

// ── MemoryValidator ──────────────────────────────────────────────────────────
// Централізована валідація даних з пам'яті.
// Замінює розпорошені перевірки isfinite/isValidPtr по всьому коду.
// ElmoreLab specifics: HP порівнюється абсолютно (hpAbs), не у відсотках.
class MemoryValidator {
public:
    struct Result {
        bool        valid = false;
        std::string error;
    };

    // Валідація PlayerState (MemReader)
    static Result validatePlayer(const MemReader::PlayerState& s);

    // Валідація L2Character (KnownList mob)
    static Result validateMob(const L2Character& mob);

    // Валідація координат гравця
    static bool isValidCoords(float x, float y, float z);

    // Валідація HP (абсолютне, ElmoreLab — без MaxHP)
    // hp >= 0, скінченне, не більше розумного максимуму
    static bool isValidHpAbs(float hp);

    // Валідація HP відсотків (тільки якщо hpMax > 0)
    static bool isValidHpPct(float hp, float hpMax);

    // Валідація кількості мобів
    static bool isValidMobCount(int count);

private:
    // L2 world bounds (Gracia Final)
    static constexpr float WORLD_XY_MIN = -327000.f;
    static constexpr float WORLD_XY_MAX =  327000.f;
    static constexpr float WORLD_Z_MIN  = -16000.f;
    static constexpr float WORLD_Z_MAX  =  16000.f;

    // Розумний максимум абсолютного HP (L2 боси мають ~2M HP)
    static constexpr float HP_ABS_MAX    = 5000000.f;
    static constexpr int   MOB_COUNT_MAX = 200;
};
