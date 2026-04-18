#pragma once
#include <cstdint>

// ── KnownList offsets (ElmoreLab Kamael client, calibrated 2026-03-30) ────────
// Дефолти відкалібровані для ElmoreLab Kamael через --calibrate + float scan.
// Перевизначаються runtime через OffsetScanner::loadOffsets("offsets.json").

// PlayerBase offsets (структура гравця — інша від загальних об'єктів!)
// Відкалібровано 2026-04-06 через --find-pos + --watch-pos:
//   0x24 = серверна позиція (стабільна, оновлюється при підтвердженні сервером)
//   0x78 = клієнтська позиція (Δ-3171 за 3.2с після кліку — але стрибає до ~0 під час анімацій!)
// ⚠ OFF_PLAYER_X МУСИТЬ = 0x24 — використовується readMobsRegionScan для дистанційного фільтра.
//   Якщо 0x78, px тимчасово ~0 → всі моби поза дистанцією → alive=0 → fake kills!
constexpr uintptr_t OFF_PLAYER_X     = 0x24;   // float — world X (серверна, стабільна)
constexpr uintptr_t OFF_PLAYER_Y     = 0x28;   // float — world Y
constexpr uintptr_t OFF_PLAYER_Z     = 0x2C;   // float — world Z
// Клієнтська позиція (для NavMesh TryRecordNavPoint — НЕ для kill detection!)
constexpr uintptr_t OFF_PLAYER_X_CLIENT = 0x78;
constexpr uintptr_t OFF_PLAYER_Y_CLIENT = 0x7C;
constexpr uintptr_t OFF_PLAYER_Z_CLIENT = 0x80;
constexpr uintptr_t OFF_KNOWN_LIST   = 0x120;  // ptr → (для Kamael — НЕ масив об'єктів!)
constexpr uintptr_t OFF_KNOWN_COUNT  = 0x124;  // (не використовується в region scan)

// L2Object offsets (від початку об'єкту, stride=0x5C0 між об'єктами)
// Калібровано --watch-pos 2026-04-18:
//   true L2Object base = addr_X - 0x90 = 0x317EA0 (підтверджено --find-pos + --watch-pos)
//   BUT: playerBase у боті = L2ObjectBase + 0x6C = 0x317f0c
//   → OFF_OBJ_X = 0x24 від playerBase (= 0x90 від true base), тип +0x5C=0 підтверджено
//   KnownList ptr відсутній (+0x88=0, +0x120=0) → тільки regionScan!
constexpr uintptr_t OFF_OBJ_ID       = 0x00;   // int32  — objectID (не відкалібровано)
constexpr uintptr_t OFF_OBJ_TYPE     = 0x5C;   // int32  — завжди 0 в цьому клієнті (не розрізняє mob/player)
constexpr uintptr_t OFF_OBJ_X        = 0x24;   // float  — world X (від playerBase = L2ObjBase+0x6C)
constexpr uintptr_t OFF_OBJ_Y        = 0x28;   // float  — world Y
constexpr uintptr_t OFF_OBJ_Z        = 0x2C;   // float  — world Z

// render_node → game_obj → HP (MR43: 2-hop via game object, підтверджено 2026-04-14)
// render_node+0x58 = ptr → game_obj; game_obj+0x14 = current HP (uint32, NOT float!)
// render_node+0x100 = interpolated X (WRONG HP offset — не використовувати для HP!)
constexpr uintptr_t OFF_GAME_OBJ_PTR = 0x58;   // render_node → game_obj ptr (uint32)
constexpr uintptr_t OFF_GAME_OBJ_HP  = 0x14;   // game_obj → current HP (uint32, NOT float!)

// L2Character extra offsets (LEGACY — використовуються лише для readMobs/readAllAsChars через KnownList ptr)
constexpr uintptr_t OFF_CHAR_HP      = 0x100;  // WARNING: reads interpolated X, NOT real HP (broken)
constexpr uintptr_t OFF_CHAR_HP_MAX  = 0x000;  // N/A    — ElmoreLab не зберігає MaxHP в KnownList
constexpr uintptr_t OFF_CHAR_IS_DEAD = 0x180;  // WARNING: reads 0x80000000 for live mobs (broken)

// L2Character extended offsets — НЕ відкалібровані. 0 = вимкнено.
// Калібрувати через --calibrate коли потрібно.
constexpr uintptr_t OFF_CHAR_MP      = 0x000;  // float  — MP (не відкалібровано)
constexpr uintptr_t OFF_CHAR_MP_MAX  = 0x000;  // float  — MaxMP (не відкалібровано)
constexpr uintptr_t OFF_CHAR_LEVEL   = 0x000;  // int32  — рівень (не відкалібровано)

// Heading персонажа. Калібрувати: стоячи нерухомо запустити --calibrate,
// потім повернутись на 90° і знову. Значення що змінилось — це heading.
// Одиниці: float радіани [-pi..pi] або int16 [0..65535] (залежить від клієнту).
constexpr uintptr_t OFF_PLAYER_HEADING = 0x30;

// Назва об'єкту — НЕ відкалібровано. 0 = вимкнено (readName не викликається).
// Калібрувати через --calibrate --name "Назва моба поряд" коли потрібно.
constexpr uintptr_t OFF_OBJ_NAME = 0x00;

// Region scan параметри (Kamael ElmoreLab, перекалібровано 2026-04-18)
// --find-pos знайшов L2Objects у діапазоні 0x317EA0..0x32D294 (stride=0x5C0).
// 0x300000-0x350000 = 320KB покриває підтверджений діапазон + буфер.
// stride=0x5C0 (1472 bytes) → 320KB / 1472 ≈ 217 object slots.
// СТАРИЙ діапазон 0x3F0000-0x500000 — НЕПРАВИЛЬНИЙ (там об'єктів немає).
constexpr uintptr_t OFF_REGION_SCAN_BASE = 0x300000; // початок сканування
constexpr uintptr_t OFF_REGION_SCAN_END  = 0x350000; // кінець
