# rdga1bot — Memory Offsets та технічні факти

## Підтверджені offsets ElmoreLab Kamael (2026-04-02)
OFF_CHAR_HP      = 0x100
OFF_CHAR_IS_DEAD = 0x180
OFF_CHAR_HP_MAX  = 0x000  (N/A — ElmoreLab не зберігає MaxHP)
OFF_OBJ_X=0x90, OFF_OBJ_Y=0x94, OFF_OBJ_Z=0x98
OFF_OBJ_TYPE=0x5C (stride=0x5C0)
Region scan: 0x3F0000-0x500000

## hpPercent()=-1 sentinel якщо MaxHP=0
isAlive() незалежна від hpMax

## HSV калібрування ElmoreLab (ManyaTheBond, 2026-03-22, пустеля)
# Див. rdga1bot.example.ini для повних значень

## Мінімапа ElmoreLab (1366×768)
# Координати в rdga1bot.example.ini → [Minimap] секція

## Клавіші — критично
F1=attack, F2=nexttarget, F7-F11=target макроси
ScrollLock=зупинка, F12=калібрування
W/S/A/D — НІКОЛИ (відкривають чат)
Рух — тільки стрілки

## XTest
Wine windows відхиляють xdotool --window → використовуємо XTest
xdotool windowfocus скидає клавіші в Wine → не використовувати

## Auto-loot
Server-side на ElmoreLab → LOOTING = ESC+300ms (без pickup key)

## Kill detection
anyMobDiedThisTick() + OpenCV hp<=2% debounce 3 тіки
UseForKillDetect=false НАЗАВЖДИ (баг: fake kills)
HP near 0% ненадійно → KillLowHpTimeoutS=8 (таймаут)
HpStableSkipBelow=5 — не кидати майже мертвих мобів

## NavMesh
CollectPoints=true → фарм 30хв → ./tools/build_navmesh → .bin
FindPath() < 1мс (тільки Detour runtime, без Recast)
