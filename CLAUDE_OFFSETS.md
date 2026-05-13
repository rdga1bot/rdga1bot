# rdga1bot — Memory Offsets та технічні факти

## Підтверджені offsets ElmoreLab Kamael (відкалібровано --watch-pos / MR70)

### L2Object / L2Character (від playerBase або render_node base)
```
OFF_PLAYER_X          = 0x24  (серверна XYZ, стабільна)
OFF_PLAYER_Y          = 0x28
OFF_PLAYER_Z          = 0x2C
OFF_PLAYER_X_CLIENT   = 0x78  (клієнтська XYZ — стрибає при click-to-move, тільки для NavMesh)
OFF_PLAYER_Y_CLIENT   = 0x7C
OFF_PLAYER_Z_CLIENT   = 0x80
```
Підтверджено: `--watch-pos` (pb+0x24 = серверна XYZ стабільна, pb+0x78 = клієнтська стрибає).

### Region scan (KnownList мобів)
```
Діапазон: 0x300000–0x350000
Stride:   0x5C0
OFF_OBJ_TYPE: 0x5C → завжди 0 в цьому клієнті (не фільтруємо)
```
Фільтри: XYZ triplet (x>200 AND y>200) + HP через render_node + dist<100 (виключаємо self).

### HP мобів — 2-hop через game_obj (MR43)
```
render_node + 0x58 → gObjPtr (uint32)
gObjPtr + 0x14     → hp_u32 (uint32, NOT float!)
gObjPtr filter:    > 0x10000 && < 0xBFFF0000
hp_u32 filter:     > 2000 (реальний моб hpAbs≈70; false positive = 3)
```
ВАЖЛИВО: `render_node+0x100` = interpolated X (НЕ HP), `render_node+0x180` = 0x80000000 (НЕ isDead).

### HP гравця — DSETUP.dll anchor (MR80, ПІДТВЕРДЖЕНО в бойових умовах)
```
Якір:         0x1003F27C (268694140 dec) — стабільна впродовж сесії
Формула:      *(anchor_addr) - 0x3DC8 = struct_base
              struct_base + 0x00 = max_hp
              struct_base + 0x08 = cur_hp
```
`mem_calib.json`:
```json
{
  "hp_anchor_addr": 268694140,
  "hp_anchor_sub": 15816,
  "hp_off": 8,
  "max_hp_off": 0
}
```
Root cause попереднього PENDING: збережено `16793212` (0x1003E7C, 7 цифр) замість `268694140` (0x1003F27C, 8 цифр).

### HP гравця — абсолютні адреси (HpAutoCalib fallback)
Якщо `mem_calib.json` відсутній — `HpAutoCalib` (blindScan-style) знаходить пари `(cur,max)` де `cur/max ≈ OCR HP%`. Зберігає `hp_abs`/`max_hp_abs` — абсолютні адреси.

**Пріоритет ReadPlayer**: `hp_abs` > `hp_anchor_addr` > `hp_off` (game_obj)

---

## HSV калібрування ElmoreLab (ManyaTheBond, 2026-03-22, пустеля)
Div. `rdga1bot.example.ini` для повних значень (`[Colors]` секція).

## Мінімапа ElmoreLab (1366×768)
```
m_minimap_roi_from_right = 185
m_minimap_roi_height     = 165
m_minimap_cx_in_roi      = 95
m_minimap_cy_in_roi      = 89
m_minimap_radius         = 78
kClosePx                 = 70px  (~ 1200 L2 units; MR46b: 35→70)
```

## Критичні правила

### Клавіші
```
W/S/A/D — НІКОЛИ (відкривають чат L2)
Рух — тільки стрілки (Up/Down/Left/Right)
ScrollLock = зупинка бота
F1=attack, F2=nexttarget, F7-F11=target макроси
```

### Auto-loot
Server-side на ElmoreLab → `LOOTING = ESC+300ms` (без pickup key).

### Kill detection
```
UseForKillDetect = false  ← НАЗАВЖДИ (баг: fake kills)
Pipeline: KnownList kl_mob_died (instant) → HP≤2% ×3 тіки → NoTarget ×8 тіків → watchdog
```

### Input backend
Wine windows відхиляють xdotool --window.  
Mouse buttons → `XTestFakeButtonEvent` (не XSendEvent — Wine ігнорує ButtonPress MR68).  
Keyboard → `XSendEvent(XKeyEvent)` — працює коректно.

## NavMesh LoA
```
navmesh_loa.pts  — 648 чистих точок
navmesh_loa.bin  — 304 полігони, 985 трикутників
L2 coords → Recast: L2_X→RX, L2_Z(висота)→RY(up), L2_Y→RZ
FindPath() < 1мс (тільки Detour runtime, без Recast при грі)
```
