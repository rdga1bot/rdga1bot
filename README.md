# rdga1bot — v1.6

C++ бот для автоматизації фарму в Lineage II.  
Протестовано: ElmoreLab Kamael/Lionna, Arch Linux, Wine/Lutris (GE-Proton), X11.

## TUI Dashboard (htop-style)

| Main — лог подій | Stats — метри |
|---|---|
| ![Main](docs/tui_main.png) | ![Stats](docs/tui_stats.png) |

| Memory — offsets + Shadow | RL — Q-model |
|---|---|
| ![Memory](docs/tui_memory.png) | ![RL](docs/tui_rl.png) |

**Керування:**
- Клік мишею по вкладці — перемикання Main / Stats / Memory / RL
- `Fn+Home` (ScrollLock) — зупинка бота
- `Fn+PgUp` (Pause/Break) — пауза / відновлення

## Результати

| Сесія | Kills | Deaths | Kill/хв |
|-------|-------|--------|---------|
| **137 хв (MR76+)** | **640** | **0** | **4.7** |
| 252 хв (MR77+) | 732 | 1* | 2.9 |
| 311 хв (MR75) | 921 | 0 | 3.0 |
| 182 хв (MR76) | 1072 | 1 | 5.9 |
| Тиждень (~54 сесії, v1.1) | ~15 700 | — | 4–15 |

\* D-лічильник v1.4 хибно показував множинні смерті через OCR HP=1% (MR79 fix)

Ціль: **kill rate > 3/хв стабільно** — стабільно досягнуто, найкраща сесія 4.7 kills/хв за 137 хв без смертей.

---

## Можливості

### Стратегічний AI (v1.6 — CATHODE-inspired)

- **Blackboard** — lock-free shared state між Director (стратегічний, 500ms) і BT (тактичний, ~100ms tick):
  - 12 float-слотів: `PLAYER_HP_PCT`, `KILLS_PER_MIN`, `MOB_DENSITY`, `ZONE_CX/CY/RADIUS`, `MOOD_INTENSITY` …
  - 6 int-слотів: `ALIVE_MOB_COUNT`, `UNREACHABLE_STREAK`, `CURRENT_MOOD`, `CURRENT_DIRECTIVE` …
  - 10 bool-слотів (bit-packed у `atomic<uint64_t>`): `HAVE_TARGET`, `CLOSE_THREAT`, `FLEE_ACTIVE`, `ZONE_VALID` …
- **DirectorSystem** — стратегічний аналіз кожні 500ms: KPM (rolling 60s), death streak, zone scoring, директиви `FARM_HERE / REPOSITION / REST_FIRST / RETURN_TO_ZONE / FLEE`
- **BotMood** — `NEUTRAL / AGGRESSIVE / CAUTIOUS / FLEE / SUSPICIOUS` з hysteresis guard (3 eval перед перемиканням, FLEE — миттєво); модулює reward shaping в RL
- **SenseSystem** — тирована якість сприйняття `MINIMAL → STANDARD → HEIGHTENED → FULL` (OCR → мінімапа → XYZ з пам'яті → KnownList)

### Бойовий цикл
- Автоматичний пошук і атака мобів через `/nexttarget` (F2) і `/target МобНейм` макроси
- Детекція смерті моба: OpenCV HP bar + KnownList memory read (instant, без debounce)
- Watchdog таймаут — перехід до лутингу якщо kill не детектується
- Pokemon sweep: `/target Pokemon` + `/useshortcut` після кожного kill
- Blacklist недосяжних мобів: HP-stable 5с → автоматичне блокування на 60с
- **MR50/51**: `m_atk_unreachable_streak` — після 5 unreachable без kills → форсуємо повний цикл; після 3 force-циклів — фізичне переміщення (break 20хв Pokemon-loop)
- **MR52**: `condNeedsRest` не відпочиває < 15с після kill; `hp_threshold` 45% (TH Vampiric Rage)
- **MR60/61**: `m_atk_mem_hp_abs` завжди оновлюється; KL-моб по hp% match (±15%) з fallback nearest
- **MR75/76**: dx stability tracking — WalkForward при stuck dx ×3; buff під час grace period

### Навігація
- Мінімапа Rotating Radar: детекція червоних/фіолетових точок мобів, ротація до цілі
- WalkForward по мінімапі (dy-based) + прогресивна ротація при застряганні
- Патруль PatrolPath (`F2000,R500,...`) при порожній мінімапі
- Memory-based навігація: координати XYZ + heading (`[Navigation] Enabled=true`)
- NavMesh Recast/Detour: `[NavMesh] Enabled=true` + зібрані точки руху
- **MR65**: force escape після >20с без руху (антизастрявання)

### Memory Reading (KnownList + MemReader)
- `process_vm_readv` — без root, без Cheat Engine, без Windows API
- `blindScan()` — автономний пошук PlayerBase; atomic abort при таймауті (MR74)
- Region scan heap: XYZ triplet scan → HP/isDead (KL_MAX_OBJECTS=2000)
- HP гравця **підтверджено** без Cheat Engine (MR80):
  - Якір DSETUP.dll: `*(0x1003F27C) - 0x3DC8 = struct_base` → `+0x00=max_hp`, `+0x08=cur_hp`
  - HpAutoCalib full-process scan fallback → `mem_calib.json`
- **MR78 `--diff-scan`**: двофазне калібрування HP/MP/CP (snapshot 1 → урон → snapshot 2 → diff)
- Thread-safe WorldState: `std::shared_mutex` — concurrent reads, exclusive writes (v1.6)
- `--dump-objects` / `--calibrate` / `--hp-calibrate` / `--watch-pos` / `--diff-scan`

### BehaviorTree планувальник
- Stackless BT VM: `BTNode` (24 bytes), `BTState` (8 bytes), плоскі масиви — без heap, без рекурсії
- Гілки: Dead → Rest → Zone → Buff → Loot → Attack → Target Selector (~22 вузли)
- Target піддерево (MR28): Init → DeadTarget → Minimap → F2AndMacro → Navigation → GeoPath → Patrol

### Huber Q-Learning / RL (`[Learning] Enabled=true`)
- Лінійна Q-функція: `Q(s,a) = W[:,a]^T * phi(s)`, 10 ознак × 6 дій
- IRLS з Huber-вагами — стійко до рідкісних смертей (викиди в reward)
- Async LearningWorker: IRLS в окремому потоці, не блокує main loop
- Mood-aware reward shaping: AGGRESSIVE/CAUTIOUS/FLEE змінюють kill_scale/death_scale
- `weights.json` — автозавантаження при старті

### Input Backend (XSendEvent Hybrid, MR66/68)
- Keyboard → `XSendEvent(XKeyEvent)` напряму до вікна гри
- Mouse buttons → `XTestFakeButtonEvent` (Wine ігнорує XSendEvent ButtonPress)
- Mouse move → XTest; ScrollLock зупинка через `XQueryKeymap` (фізичний стан)
- `[Input] Backend = hybrid | xtest | xsendevent` в INI

### QA Monitor
- `launch_qa.sh` — єдиний скрипт запуску: бот + frame capture + video record
- `qa/frame_capture.py` — зовнішній демон: tail log → scrot при ключових подіях (MR62)
- `qa/video_record.py` — ffmpeg x11grab запис + нарізка кліпів (MR63)
- `qa/qa_monitor.py` — IsolationForest аномалії, death_loop, kl_hp_spike

---

## Швидкий старт

```bash
# Залежності (Arch Linux)
sudo pacman -S opencv gcc libx11 libxtst libxext curl ncurses cmake ninja

# Зібрати (два варіанти)
bash build.sh                    # старий Makefile-стиль
cmake --preset release && ninja -C build    # CMake (рекомендовано)

# Запуск
./launch.sh          # TUI налаштувань
./rdga1bot --quick   # без TUI (rdga1bot.ini)

# Запуск з QA (бот + frame capture + video)
./launch_qa.sh
```

## Build система (CMake Presets)

| Preset | Папка | Опції | Час збірки |
|--------|-------|-------|-----------|
| `release` | `build/` | `-O2` + Link-Time-Opt | ~5 хв |
| `debug-fast` | `build_debug/` | `-O0 -g` | ~2 хв |
| `qa-debug` | `build_qa/` | ASan + UBSan + AllocStats | ~15 хв |

```bash
cmake --preset debug-fast && ninja -C build_debug   # швидка розробка
cmake --preset qa-debug   && ninja -C build_qa      # перед MR (sanitizers)
```

**AllocStats** (`qa-debug`): глобальний `operator new/delete` з атомарними лічильниками — логує загальну кількість алокацій кожні ~60с (`[AllocStats]` рядок у лозі).

## Конфігурація

Копіювати `rdga1bot.example.ini` → `rdga1bot.ini`.

```ini
[Learning]
Enabled = true        # Huber Q-Learning + mood-aware reward

[KnownList]
Enabled = true
AutoScan = true

[MemReader]
Enabled = true
UseKLBase = true      # координати з playerBase (без Cheat Engine)
PosX_Offset = 0x24
PosY_Offset = 0x28
PosZ_Offset = 0x2C
ShadowMode = true     # логувати OCR vs Memory

[Potions]
HpThreshold = 45      # TH Vampiric Rage: лікується атакуючи
```

Повний список: [`rdga1bot.example.ini`](rdga1bot.example.ini)

## Калібровка

```bash
# Двофазне авто-калібрування HP/MP/CP (без Cheat Engine):
./rdga1bot --diff-scan
# → Snapshot 1 (введи HP%), отримай урон, Enter → Snapshot 2 → mem_calib.json

./rdga1bot --calibrate              # дамп KnownList об'єктів
./rdga1bot --calibrate --name "Mob" # пошук по імені
./rdga1bot --find-pos               # пошук PlayerBase XYZ offset
./rdga1bot --watch-pos              # live monitor XYZ при русі
```

## Архітектура

```
Brain.cpp/.h                — диспетчер: сприйняття + потіони + BotBehaviorTree dispatch
BehaviorTree.h/.cpp         — stackless BT VM (BTNode 24B, BTState 8B)
BotBehaviorTree.h/.cpp      — Farm BT ctor/init/tick/reset/conditions (~273 рядки)
BotBT_Dead/Buff/Attack/Target/Nav/RL.cpp  — гілки BT (6 файлів)

src/Blackboard.h/.cpp       — lock-free shared state (atomic float/int/bool)
src/DirectorSystem.h/.cpp   — стратегічний AI, 500ms cadence, mood+directive
src/BotMood.h               — NEUTRAL/AGGRESSIVE/CAUTIOUS/FLEE/SUSPICIOUS + MoodManager
src/SenseSystem.h           — MINIMAL→FULL tiered perception

Eyes.cpp/.h                 — OpenCV: HP/MP/CP, target HP, мінімапа, NPC detect
Hands.h                     — дії: keyboard/mouse через Intercept
Intercept_Linux.cpp         — XSendEvent hybrid backend
platform.h                  — pid_t/ssize_t (Linux + Windows)
ProcessMemory.h             — process_vm_readv (Linux) / ReadProcessMemory (Windows)

Config.cpp/.h               — INI парсер + interactive TUI
Dashboard.cpp/.h            — ncurses / PDCurses TUI
MemReader.cpp/.h            — HP/MP/CP/XYZ гравця; UseKLBase + AutoCalibratePlayer
KnownListReader             — region scan мобів
WorldState                  — thread-safe агрегатор (shared_mutex, bgLoop 1Hz)

LinearQModel.h/.cpp         — Q(s,a)=W^T*phi(s), IRLS+Huber, 6 дій
LearningWorker.h/.cpp       — async IRLS batch update thread
FeatureExtractor.h          — phi(s): 10 ознак → Eigen::VectorXf
ShadowLogger.h/.cpp         — A/B Memory vs OCR JSONL лог

src/tools/diag_*.cpp        — --calibrate, --diff-scan, --map, --find-pos …
src/AllocStats.h/.cpp       — operator new/delete counters (qa-debug preset)
.github/workflows/ci.yml    — CI: ubuntu-22.04, debug-fast build + run-san tests
```

## Мультипоточність

| Thread | Core | Робота |
|--------|------|--------|
| Main | 1 | Brain tick + BT + DirectorSystem (~100ms) |
| VisionWorker | 2 | async DetectNPCs + DetectMinimap |
| GeodataWorker | 3 | async A* FindPath |
| NavMeshWorker | 4 | async Detour FindPath |
| LearningWorker | — | async IRLS batch update |
| WorldState bgLoop | — | KnownList region scan 1Hz |

WorldState: `std::shared_mutex` — читачі (`mobs()`, `aliveCount()` тощо) не блокують один одного; записувач (bgLoop) бере exclusive lock.

## Клавіші

| Клавіша | Дія |
|---------|-----|
| ScrollLock | Зупинити бот (глобально) |
| P | Пауза / продовження |
| S | Налаштування (hot-reload) |
| R / Space | Скинути детекцію HP/MP/CP барів |
| F12 | Зберегти calibrate_*.png |

## Тести та CI

```bash
make -C tests run       # всі юніт-тести (потрібен gtest)
make -C tests run-san   # ASan + UBSan (test_blackboard + test_director)
```

CI (GitHub Actions, `ubuntu-22.04`): cmake `debug-fast` → ninja → `run-san`.

## Windows Port (MR67)

- `ProcessMemory.h`: `ReadProcessMemory`, Toolhelp32, `EnumProcessModules`
- `VirtualQueryEx` замість `/proc/maps` (OffsetScanner + KnownListReader)
- `CMakeLists.txt`: cross-platform split (Linux: X11/ncurses; Windows: psapi/PDCurses/interception)

```bash
set INTERCEPTION_DIR=C:\interception
cmake -B build -G "Visual Studio 17 2022" .
cmake --build build --config Release
```

## Вимоги

### Linux
- X11 (не Wayland), Wine/GE-Proton (Flatpak Lutris)
- `g++` C++17, OpenCV 4.x, ncurses, libx11, libxtst, libxext
- cmake + ninja (рекомендовано) або bash build.sh
- Python 3.x + scikit-learn + scrot + ffmpeg (QA, опціонально)
- googletest (тести, опціонально)

### Windows (нативний порт)
- Windows 10 x64, MSVC 2022 або MinGW-w64, C++17
- OpenCV 4.x prebuilt, [interception driver](https://github.com/oblitum/Interception)
- PDCurses (опціонально), curl.exe (вбудовано з Windows 10 1803)
