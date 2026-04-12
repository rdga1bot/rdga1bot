#include "MemoryValidator.h"
#include <cmath>
#include <sstream>

MemoryValidator::Result MemoryValidator::validatePlayer(
        const MemReader::PlayerState& s) {
    Result r;
    if (!s.valid) {
        r.error = "PlayerState.valid=false";
        return r;
    }
    if (s.hp < 0 || s.max_hp < 0) {
        r.error = "HP/MaxHP negative: hp=" + std::to_string(s.hp)
                + " max=" + std::to_string(s.max_hp);
        return r;
    }
    // max_hp=0 дозволено (ElmoreLab не зберігає MaxHP для мобів),
    // але для гравця max_hp має бути > 0
    if (s.max_hp > 0 && s.hp > s.max_hp) {
        r.error = "HP > MaxHP";
        return r;
    }
    if (!isValidCoords(s.x, s.y, s.z)) {
        std::ostringstream oss;
        oss << "Invalid coords: x=" << s.x << " y=" << s.y << " z=" << s.z;
        r.error = oss.str();
        return r;
    }
    r.valid = true;
    return r;
}

MemoryValidator::Result MemoryValidator::validateMob(const L2Character& mob) {
    Result r;
    if (!isValidCoords(mob.x, mob.y, mob.z)) {
        std::ostringstream oss;
        oss << "Invalid mob coords: x=" << mob.x
            << " y=" << mob.y << " z=" << mob.z;
        r.error = oss.str();
        return r;
    }
    // ElmoreLab: hpMax=0 — нормально. Перевіряємо тільки абсолютне HP.
    if (!isValidHpAbs(mob.hp)) {
        r.error = "Invalid hpAbs: " + std::to_string(mob.hp);
        return r;
    }
    r.valid = true;
    return r;
}

bool MemoryValidator::isValidCoords(float x, float y, float z) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
        return false;
    if (x < WORLD_XY_MIN || x > WORLD_XY_MAX) return false;
    if (y < WORLD_XY_MIN || y > WORLD_XY_MAX) return false;
    if (z < WORLD_Z_MIN  || z > WORLD_Z_MAX)  return false;
    return true;
}

bool MemoryValidator::isValidHpAbs(float hp) {
    return std::isfinite(hp) && hp >= 0.f && hp <= HP_ABS_MAX;
}

bool MemoryValidator::isValidHpPct(float hp, float hpMax) {
    if (hpMax <= 0.f) return false; // немає MaxHP — відсоток невідомий
    return hp >= 0.f && hp <= hpMax;
}

bool MemoryValidator::isValidMobCount(int count) {
    return count >= 0 && count <= MOB_COUNT_MAX;
}
