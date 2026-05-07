# rdga1bot — Контекст для Claude Code

## Середовище
- OS: Arch Linux (CachyOS), X11, користувач rdga1
- Гра: Lineage 2, ElmoreLab (Kamael/Lionna), персонаж ManyaTheBond (Treasure Hunter/Dagger)
- Клієнт: Flatpak Lutris + GE-Proton/Wine
- Папка: /home/rdga1/rdga1prj/l2net/
- Build: bash build.sh && ./launch.sh

## Поточний стан (2026-05-07)
- **BotBehaviorTree** — єдиний планувальник. RL: `[Learning] Enabled=true` за замовчуванням.
- **MR80** — `HpAutoCalib`: диференційне авто-калібрування HP offset (замінює MR54 AutoCalibratePlayer)
  - Фаза 1 (Searching): збирає кандидати (cur_off, max_off) де cur/max ≈ OCR HP% при HP < 95%
  - Фаза 2 (Validating): відкидає кандидатів що не відстежують зміну OCR HP диференційно (±5%+2)
  - Фаза 3 (Confirmed): 3 підтвердження → зберігає у mem_calib.json → ShadowMode стає корисним
  - Захист від false positives: max_hp ∈ [50,20000] + диф.перевірка + max 20 спроб
  - Лог: `[AutoCalib] Фаза 1: N кандидатів` → `[AutoCalib] Спроба K/20` → `[AutoCalib] ПІДТВЕРДЖЕНО`
- **MR26** — MemoryValidator + blindScan(timeoutMs) + ShadowLogger (A/B JSONL, ShadowMode=false)
- **MR27** — actTarget → 6 приватних `tgtHandle*` instance methods
- **MR28** — Target Selector піддерево з 7 BT вузлів (~22 вузли загалом)
- **MR29** — RL активовано (`[Learning] Enabled=true`); condNeedsRest + tgtHandlePatrolAndRotate overrides
- **MR30** — RL logs → Brain::Log() через LogFn callback; weights.json збереження при shutdown
- **MR31** — condIsDead: `!inGrace()` запобігає дублюванню notifyDeathRL після respawn memory lag
- **MR32** — blindScan coordinate filter: `|X|<1000 && |Y|<1000` (fix для ToI/LoA де X<30000)
- **MR33** — loadWeights() двопрохідна валідація num_features/num_actions перед завантаженням
- **MR34** — RL повні overrides для всіх 6 дій + softmax confidence ∈ (0,1] (fix від'ємних Q)
- **MR35** — playerBaseCache з offsets.json використовується до blindScan (Attempt 0)
- **MR36** — видалено Options.cpp/.h (legacy dead code від l2cvbot)
- **MR37** — FeatureLogInterval: `[RL-F]` рядок у session log кожні N тіків (default 300)
- **MR38** — blindScan reliability fix + scanmem-style maps parser
- **MR39** — scanmem-style region types + pread + priority scan
- **MR41** — CE reverse scan → forward scan; HP>0 filter + топ-30 ліміт
- **MR42** — діагностика: render_node дамп + game_obj pool сканування + range ptr scan
- **MR43** — **HP FIX**: render_node+0x58 → game_obj → +0x14 = HP (uint32, NOT float!)
  - render_node+0x100 = interpolated X (не HP!), render_node+0x180 = 0x80000000 (не isDead!)
  - `isAlive()` тепер повертає `true` для живих мобів; kill detection виправлено
- **NavMesh tools** — `scripts/navmesh_preview.py` (2D), `scripts/navmesh_3d.py` (Detour binary → 3D mesh)
- **NavMesh LoA** — `navmesh_loa.pts` (648 чистих точок), `navmesh_loa.bin` (304 полігони, 985 трикутників)
  - Координатне відображення: L2_X→Recast_X, L2_Z(висота)→Recast_Y(up), L2_Y→Recast_Z
- **MR46b** — зупинка атаки по dist (не minimap_dots); kClosePx: 35→70px
- **MR47** — `m_buff_after_death`: форс-баф після respawn ігнорує minimap_close_threat
- **MR48** — KL-HP: nearest-mob (не min-HP); логування `[KL-HP]`; видалено false `has_target` override
- **MR49** — QA stats фільтрація по сесії (session_start з filename + uptime/kills reset detector)
  - `summarize()` коректно з max() після фільтрації однієї сесії
  - `log_tailer.py`: kl_hp / kl_hp_pct патерни; `anomaly_engine.py`: kl_hp детектори
- **MR50** — `m_atk_unreachable_streak`: після 5 послідовних unreachable без kills →
  ігноруємо minimap_close_threat, форсуємо повний цикл (Pokemon macro + patrol)
- **MR51** — `m_atk_streak_force_count`: після 3 force-циклів (~75с) → ESC + `has_target=false`
  → patrol сам рухається та знаходить нову ціль (break 20хв Pokemon-loop)
  - QA: death_loop рахує лише "Фаза 0" (1 смерть = 4 [DEAD] рядки → fix false CRITICAL)
- **MR52** — два критичних фікси:
  - `condNeedsRest`: не відпочивати < 15с після kill (активна зона бою); hp_threshold 70%→45%
    - Root cause смерті: атакер > 70px minimap → close_threat=false; потіон стабілізує HP → hp_falling=false → бот застряє в Rest 51с і гине
    - RL-F "minimap=1.0" = кількість dots (feature[5]=dots.size()/5), НЕ minimap_close_threat!
  - KL-HP coords fallback: якщо `m_mem_player.valid=false` але `m_player_base` є → `refreshPlayerXYZ()` → `gs.coords_valid=true`
    - PosX_Offset=0x0 в INI → MemReader вимкнений → coords_valid=false → KL-HP ніколи не запускався
    - Тепер читає XY гравця прямо з playerBase (той самий offset що KnownList)
- **MR53** — MemReader + ShadowMode увімкнено для тестування:
  - `UseKLBase=true`: MemReader отримує playerBase від KnownList (без статичного PlayerPtr)
  - `PosX/Y/Z_Offset=0x24/0x28/0x2C`: XY координати з пам'яті валідні → `m_mem_player.valid=true`
  - HP_Offset=0x0 (не відкалібровано) → HP з OCR; coords з memory → coords_valid=true через MemReader
  - `ShadowMode=true`: логує порівняння OCR vs Memory → дозволяє побачити розбіжності
  - HP/MP/CP offsets знайти пізніше через Cheat Engine або `--calibrate`
- **MR54** — MemReader авто-калібрування HP/MP/CP без Cheat Engine:
  - `AutoCalibratePlayer(pid, playerBase, hp%, mp%, cp%)`: сканує +0x00..+0x300, шукає пари
    (cur, max) де cur/max*100 ≈ OCR відсоток (±4%)
  - Тригер: після KL знаходить playerBase + OCR HP стабільний 3 тіки + хоча б 1 стат < 98%
  - Результат → `mem_calib.json` (наступний запуск завантажує автоматично)
  - Якщо HP/MP/CP=100% → чекає до 6с (60 тіків) потім калібрує (менш точно)
  - Логи: `[MemCalib] HP cur=+0xXX max=+0xYY` тощо
- **MR55** — KL-HP: фільтр dist<100 (виключає self/player з вибірки мобів) + throttle логу при зміні цілі
- **MR58** — `actTgtF2AndMacro`: SKIP коли `has_target=1 && streak=0`; додано `[F2] SEND/SKIP` логування
- **MR60** — `m_atk_mem_hp_abs = absHp` завжди (не тільки при hpMax==0):
  - Root cause false stuck_streak3: при активному DPS damage <1% HP → int% не змінюється → 5с таймер спрацьовував
- **MR61** — KL-HP вибір моба по hp% match (±15% від OCR) з fallback на nearest:
  - Раніше: nearest-by-dist міг обрати іншого моба (B), B.HP незмінний → false unreachable
  - Тепер: primary = моб де |kl.hpPercent - ocr_hp| < 15%; fallback = nearest
- **MR62** — QA frame capture вилучено з бота → зовнішній демон `qa/frame_capture.py`:
  - `frame_capture.py`: tail session_*.log → scrot при [LOOTING]/[DEAD]/[NAV]/[ATTACKING]
  - Бот не знає про QA — принцип розділення відповідальності
- **MR63/63b** — `qa/video_record.py`: ffmpeg x11grab запис сесій + нарізка кліпів
  - `record` режим: → `qa/videos/farm_YYYYMMDD_HHMMSS.mkv`
  - `clip` режим: парсинг log + `ffmpeg -ss -t -c copy` для кожної події
  - Авто-детект вікна L2 через xdotool (X=1616, Y=209, 1366×768)
- **MR64/64b/64c/64d** — `launch_qa.sh`: єдиний старт бот + frame_capture + video_record
  - Bot stdout → `tee logs/session_YYYYMMDD_HHMMSS.log` (файл створюється в момент старту)
  - `_CLEANED` guard запобігає double-cleanup при Ctrl+C; `sleep 3` для ffmpeg flush
  - trap: `HUP` для закриття вікна терміналу; `INT/TERM` для Ctrl+C
- **MR65** — `tgtHandlePatrolAndRotate`: force escape після >20с без руху (стіна + "Ціль не видно"):
  - `m_tgt_last_walk_time` + `m_tgt_nav_prev_was_walk` → якщо >20с без WalkForward → форсуємо WalkForward(1500)
  - Root cause смерті наприкінці сесії: `IsGroundAhead()=false` → нескінченна ротація без переміщення
- **MR66** — XSendEvent hybrid input backend (Linux):
  - Keyboard → `XSendEvent(XKeyEvent)` до вікна гри (без глобального grab)
  - Mouse move → XTest (DirectInput ігнорує XSendEvent MotionNotify)
  - Mouse buttons → `XSendEvent(XButtonEvent)` з window-relative координатами
  - `[Input] Backend = hybrid` в INI; `SetBackend()`/`SetWindowRect()` в Intercept
  - `platform.h` — централізовані платформо-залежні типи (pid_t, ssize_t)
  - ScrollLock зупинка: XQueryKeymap читає фізичний стан → не залежить від XSendEvent
- **MR67** — Windows порт Steps 2-11:
  - `ProcessMemory.h`: `ReadProcessMemory` branch для Windows
  - `MemReader.cpp`: `FindPid` через Toolhelp32; `FindModuleBase` через `EnumProcessModules`
  - `offset_scanner.cpp`: `getReadableRegions()` через `VirtualQueryEx`
  - `knownlist_reader.cpp`: `refreshScanCache()` через `VirtualQueryEx`
  - `Notify.cpp`: `SendSync()` через `CreateProcess`/`curl.exe`; POSIX fork guarded
  - `main.cpp`: `<dirent.h>` guarded; `findL2Pid()` Windows branch; inline lambda → `findL2Pid()`
  - `CMakeLists.txt`: cross-platform split (Linux/Windows sources + libs); PDCurses + interception
  - `Capture.cpp`, `Window.cpp` вже існували (Steps 6-7 виконані раніше)
- **MR68** — Hybrid mouse buttons → XTest (Wine ігнорує XSendEvent ButtonPress):
  - Root cause: `XSendEvent(XButtonEvent)` → Wine ігнорує у більшості GE-Proton версій
  - Виправлено: mouse buttons завжди через `XTestFakeButtonEvent` навіть у Hybrid режимі
  - Keyboard залишається через `XSendEvent(XKeyEvent)` — працює коректно
- **MR69** — `--watch-pos`: alive heartbeat щосекунди (виводить поточні значення навіть без руху):
  - Дозволяє підтвердити що offset читається правильно без необхідності рухати персонажа
- **MR70** — перекалібровка L2Object offsets + region scan через `--find-pos`/`--watch-pos`:
  - `OFF_OBJ_X/Y/Z`: `0x90→0x24` від playerBase (true L2ObjBase+0x6C → зсув 0x6C підтверджено)
    - `--watch-pos` підтвердив: `pb+0x24` = серверна XYZ (стабільна); `pb+0x78` = клієнтська (стрибає)
  - Region scan: `0x3F0000-0x500000 → 0x300000-0x350000` (фактичний діапазон підтверджено)
    - `--find-pos` знайшов L2Objects у `0x317EA0..0x32D294`, stride=0x5C0 на 6+ об'єктах
  - `OFF_OBJ_TYPE=0x5C` — завжди 0 в цьому клієнті (не розрізняє mob/player/item)
  - KnownList ptr відсутній (`+0x88=0`, `+0x120=0`) → тільки regionScan
  - `cache_valid`: AND-логіка `cx>200 AND cy>200` (проти false positive Y=0 з Wine .data секції)
- **MR71** — cleanup: виключення непідтверджених offsets і мертвого коду:
  - `OFF_OBJ_NAME/LEVEL/MP/MP_MAX → 0` (не відкалібровані — читали garbage)
  - `readMobsRegionScan`: прибрано OFF_OBJ_TYPE фільтр (завжди 0, dead filter)
  - `world_state`: прибрано `readAllAsChars` fallback (KnownList ptr=0 → завжди empty)
  - `world_state`: прибрано `readItemsRegionScan` (type==2 ніколи не matches + server-side loot)
  - `offsets.json`: прибрано LEGACY OFF_CHAR_HP/HP_MAX/IS_DEAD (dead code paths)
  - Реальні фільтри: XYZ triplet + HP через render_node + dist<100 (MR55)
- **MR72** — два критичних фікси після першого бойового тесту:
  - `Brain.cpp safePct()`: якщо memory pct поза [0,100] → OCR fallback (mem_calib.json offset 612 читав interpolated X → HP=254)
  - `[Shadow] spam fix`: `m_consecutive_mem_fails >= threshold` → `== threshold` (логуємо тільки раз, не кожен тік)
  - `mem_calib.json` видалено (false-positive AutoCalibrate: render_node+0x264 = interpolated X, не HP)
  - Root cause TUI jitter + HP=254: safePct guardrail тепер захищає від будь-якого false-positive calib
- **MR73** — WalkForward вилучено з GeoNav + HP filter 500k→2k:
  - `tgtHandleGeoNavigation`: видалено WalkForward за `unreachable_streak > 2`
  - Root cause: KL-HP false positives (hp_u32<500k = 157 "мобів") → stable HP 5s → streak++ → крокував від моба після 3-5 атак
  - `knownlist_reader`: `hp_u32 > 500000 → > 2000` (real mob hpAbs=70, підтверджено логами)
  - Реальний вихід зі застрягань залишився: tgtHandlePatrolAndRotate MR65 (20с без руху)
- **MR74** — crash при shutdown: `std::thread(...).detach()` blindScan → use-after-free при знищенні kl_scanner.
  Fix: kl_scan_thread (join, не detach) + OffsetScanner::abortScan() + join при bot_exit.
- **MR75** — аналіз сесії (921 kills, 0 deaths, 3h) → 5 виправлень:
  1. `tgtHandleMinimap`: dx stability tracking — WalkForward без IsGroundAhead якщо dx застряг 3+ ротації
     Root cause 315с gap: dx=71 (1px вище kClosePx=70) → нескінченна RotateRight без прогресу
  2. KL-HP false positive: `absHp < 10 && hpMax=0` → ігноруємо (false positive hpAbs=3 → 139 false unreachable)
  3. ShadowLogger::logMobComparison: `m_totalComparisons++` fix (diff > cmp раніше)
  4. rdga1bot.ini: HP_Threshold 70→45 (MR52 TH Vampiric Rage)
  5. PERF slow ticks (300-656мс) → inherent від ESC+200ms; покращиться з Priority 1+2
- **Наступні пріоритети**: live farm тест MR75; перевірити KL-HP events (очікується > 1 за сесію)

## Критичні правила (НІКОЛИ не порушувати)
- W/S/A/D — НЕ використовувати (відкривають чат L2), рух тільки стрілками
- UseForKillDetect=false НАЗАВЖДИ (баг: fake kills)
- Auto-loot server-side: LOOTING = ESC+300ms (без pickup key)
- XSendEvent hybrid backend для key/mouse injection (не xdotool --window)
- ScrollLock = зупинка бота (через XQueryKeymap — читає фізичний стан, не залежить від XSendEvent)
- Нові фічі: завжди feature flag в INI (Enabled=false за замовчуванням)
- Мінімальні цільові зміни — не робити broad rewrites
- BT tick() сигнатура: `std::string tick(GameState& gs)` — НЕ міняти
- m_children[] масив — НЕ переставляти під час виконання (ламає BTState)

## Ключові файли
- src/Brain.cpp               — координатор (сприйняття + потіони + dispatch)
- src/BotBehaviorTree.h/.cpp  — Farm BT + RL + Target піддерево (MR27/28/50-52/58/60/61/65)
- src/BehaviorTree.h/.cpp     — Stackless VM (BTNode 24B, BTState 8B)
- src/game_state.h            — GameState struct
- src/MemoryValidator.h/.cpp  — валідація PlayerState/L2Character/coords (MR26)
- src/ShadowLogger.h/.cpp     — A/B Memory vs OCR → JSONL лог (MR26)
- src/LinearQModel.h/.cpp     — Q(s,a)=W^T*phi(s), IRLS+Huber, 6 дій
- src/LearningWorker.h/.cpp   — async IRLS thread
- src/FeatureExtractor.h      — 10 ознак GameState → Eigen::VectorXf
- src/ExperienceBuffer.h      — циклічний буфер Experience
- src/RewardCalculator.h      — reward function
- src/offsets_config.h        — ElmoreLab Kamael offsets (відкалібровано)
- src/Intercept.h             — cross-platform input interface; LinuxInputBackend enum (MR66)
- src/Intercept_Linux.cpp     — XSendEvent hybrid backend (MR66)
- src/platform.h              — платформо-залежні типи: pid_t, ssize_t (MR66)
- src/ProcessMemory.h         — ReadProcessMemory (Win) / process_vm_readv (Linux) (MR67)
- third_party/eigen/          — Eigen 3.4.0 header-only
- build.sh                    — компіляція (g++, без cmake)
- CMakeLists.txt              — cross-platform CMake (Linux + Windows, MR67)
- rdga1bot.example.ini        — всі опції з коментарями
- launch_qa.sh                — єдиний старт: бот + frame_capture + video_record (MR64)
- qa/qa_monitor.py            — QA daemon (IsolationForest + session filtering)
- qa/frame_capture.py         — зовнішній демон скріншотів (MR62)
- qa/video_record.py          — ffmpeg запис/нарізка сесій (MR63)

## НЕ включати в білд
Runloop.cpp — legacy від l2cvbot (Options.cpp/.h видалено в MR36)

## MemPalace — контекст між сесіями
На початку кожної сесії:
  mp --palace ~/l2bot/rdga1bot/memory/palace wake-up
Перед кожним MR:
  ./scripts/mp-snapshot.sh "опис змін"

## Детальний контекст (читай за потребою)
- Архітектура + BT дерево + Config INI: cat CLAUDE_ARCH.md
- Історія MR1-MR25:                    cat CLAUDE_HISTORY.md
- Memory offsets + технічні факти:     cat CLAUDE_OFFSETS.md
