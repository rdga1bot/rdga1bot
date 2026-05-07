# CLAUDE.md — rdga1bot Project Constitution v2.0
# Єдиний файл: промпт + архітектура + операційний протокол
# Розташування: корінь rdga1bot/CLAUDE.md
# Версія: 2.0 (ADAPTED FROM v1.0) | Дата: 2026-04-16

```
┌─────────────────────────────────────────────────────────────┐
│ ЦЕЙ ФАЙЛ — ЄДИНЕ ДЖЕРЕЛО ПРАВДИ ДЛЯ ВСІХ AI-АСИСТЕНТІВ   │
│ • Не пояснюй • Не вчи • Пиши код • Дотримуйся правил       │
│ • Порушення §1.2 = // ERROR: VIOLATES CONSTRAINT [rule]     │
└─────────────────────────────────────────────────────────────┘
```

> **Протокол помилкового запиту**: Якщо запит ≠ формату §7.1 → відповідай лише:
> `[CLAUDE.md] Запит не відповідає формату §7.1. Використайте: PROJECT: rdga1bot | PHASE: ... | TASK: ...`

---

## §0 · КОНТЕКСТ ПРОЄКТУ

```
Проєкт:  rdga1bot — L2 farm bot (OpenCV + Memory + BehaviorTree + RL)
Гравець: ManyaTheBond (Treasure Hunter/Dagger), ElmoreLab Kamael/Lionna
Залізо:  Arch Linux (CachyOS) · X11 · Wine/GE-Proton (Flatpak Lutris)
Мова:    C++17 (БЕЗ C++20, БЕЗ C)
Vision:  OpenCV 4.x + XShm screen capture (lazy HSV, ~30-60 FPS)
Input:   XTest (XTestFakeKeyEvent/ButtonEvent) — НЕ xdotool --window
Memory:  process_vm_readv (KnownList scan, blindScan, no root required)
AI:      BehaviorTree stackless VM (24-byte ноди, MAX_NODES=256)
RL:      Huber Q-Learning (Enabled=false default, 10 features × 6 actions)
Nav:     Recast/Detour NavMesh + L2J Geodata A*/JPS+ (обидва опціональні)
Config:  власний INI-парсер + weights.json для RL (двопрохідна валідація)
Build:   bash build.sh (без cmake)
Debug:   ncurses TUI (Dashboard.cpp) — завжди активний
Папка:   ~/l2bot/rdga1bot/
Фаза:    MR44+
Репо:    https://github.com/rdga1bot/rdga1bot
```

---

## §1 · БІЛД

```bash
# Основний спосіб:
bash build.sh              # чистий release білд
./rdga1bot --quick         # запуск без TUI setup
./rdga1bot --config rdga1bot.ini

# Спеціальні режими:
./rdga1bot --calibrate                    # HSV + KnownList calibration
./rdga1bot --calibrate --name "MobName"  # з пошуком назви моба
./rdga1bot --dump-objects                 # дамп KnownList структури
./rdga1bot --find-pos                     # пошук XYZ offset у пам'яті
./rdga1bot --watch-pos [--pb 0xADDR]     # live monitor координат
./rdga1bot --discover-klist              # CE-style KnownList autodiscover
./rdga1bot --hp-calibrate                # HP/isDead offset пошук
./rdga1bot --map [--pb 0xADDR]           # запис NavMesh точок вручну
./rdga1bot --scan-pos                    # скан XYZ через region scan

# CMakeLists: НЕ ВИКОРИСТОВУЄТЬСЯ — тільки build.sh
# build.sh підхоплює всі .cpp в корені проекту автоматично
```

---

## §2 · СТЕК І ЗАБОРОНИ

### §2.1 · Технічний стек (НЕЗМІННИЙ)

| Компонент | Рішення | Примітка |
|-----------|---------|----------|
| Мова | C++17 | Без C++20, без C |
| Vision | OpenCV 4.x + XShm | Lazy HSV, template matching для бафів |
| Input | XTest (libxtst) | XTestFakeKeyEvent/ButtonEvent |
| Memory | process_vm_readv | /proc/[pid]/mem, no ptrace, no root |
| AI | BehaviorTree.h/.cpp | Stackless VM, 24-byte ноди, MAX_NODES=256 |
| Nav | Recast/Detour + L2J .geo | A*/JPS+, обидва опціональні |
| Config | власний INI-парсер | strstr + stol, без зовнішніх залежностей |
| JSON (RL) | власний міні-парсер | тільки weights.json |
| Debug UI | ncurses (Dashboard.cpp) | завжди активний, 3 колонки + RL |
| Logging | std::cerr + logfile | async-safe |
| Notifications | fork+curl → Telegram Bot API | без libcurl |
| Лінійна алгебра | Eigen 3.4.0 (header-only) | тільки для RL |

### §2.2 · Абсолютні заборони

| Заборонено | Причина | Альтернатива |
|-----------|---------|-------------|
| W/S/A/D для руху | відкривають чат в L2! | тільки стрілки ↑↓←→ |
| Cheat Engine / root | детект сервером, бан | process_vm_readv, blindScan |
| xdotool --window | Wine відхиляє | XTest напряму |
| UseForKillDetect=true | fake kills баг | KnownList isDead + hp<=0 |
| рекурсивний BT | stack overflow | stackless VM, ітеративний |
| Wayland | XShm/XTest несумісні | тільки X11, перевірка $DISPLAY |
| ImGui у релізі | overhead, зайні залежності | ncurses TUI завжди |
| std::vector у frame loop | heap alloc у critical path | фіксовані масиви [MAX_N] |
| assert() без fallback | crash у production | TraceLog + return default |
| мутація WorldState під BT | data race з perception | snapshot copy під mutex |
| KnownList без isValidPtr | segfault | strict guard > 0x10000 |
| XTest без RandomDelay | анти-чіт детект | normal distribution delays |
| malloc/free у Vision loop | latency spike | пули, static buffers, pre-alloc |
| float для HP порівняння | Kamael HP = uint32 | порівняння цілих чисел |
| CMakeLists | не використовується | bash build.sh |

### §2.3 · Критичні константи (НІКОЛИ не змінювати без профайлінгу)

```cpp
// BehaviorTree
MAX_NODES    = 256   // BTNode 24B × 256 = 6KB (cache-friendly)
MAX_CHILDREN = 512   // flat children array
BTNode size  = 24B   // cache-line aligned, ПЕРЕВІРЕНО static_assert

// KnownList
KL_MAX_OBJECTS = 2000  // L2 Kamael: ~500-1500 мобів
isValidPtr(v): v > 0x10000 && v < 0xBFFF0000  // Wine 32-bit user space

// RL
NUM_FEATURES = 10    // FeatureExtractor → Eigen::VectorXf
NUM_ACTIONS  = 6     // TargetNearest/Weighted/NavMemory/Patrol/RestNow/BuffNow

// ElmoreLab Kamael offsets (offsets_config.h)
OFF_PLAYER_X    = 0x24   // серверна позиція (стабільна)
OFF_KNOWN_LIST  = 0x120  // KL container ptr
OFF_GAME_OBJ_PTR = 0x58  // render_node → game_obj (2-hop HP)
OFF_GAME_OBJ_HP  = 0x14  // game_obj → HP (uint32, NOT float!)
```

---

## §3 · АРХІТЕКТУРА

### §3.1 · Файлова структура (ФАКТИЧНА, поточний проект)

```
rdga1bot/
├── CLAUDE.md                ← ЦЕЙ ФАЙЛ
├── main.cpp                 ← entry point + CLI args + signal handlers
├── Brain.h/.cpp             ← dispatcher: perception → BT → actions
├── Config.h/.cpp            ← INI parser + validation + TUI editor
├── BehaviorTree.h/.cpp      ← stackless VM (BTNode 24B, BTState 8B)
├── BotBehaviorTree.h/.cpp   ← Farm BT + Target subtree (7 вузлів) + RL
├── Eyes.h/.cpp              ← OpenCV: HP bars, minimap, NPC, template
├── Hands.h                  ← XTest keyboard/mouse actions
├── Input.h/.cpp             ← XTest backend
├── Intercept.h/.cpp/.Linux  ← XTest platform implementation
├── Capture.h/.cpp/.Linux    ← XShm screen capture + ROI optimization
├── Window.h/.cpp/.Linux     ← X11 window finding (кешований)
├── MemReader.h/.cpp         ← HP/MP/CP/XYZ з пам'яті (process_vm_readv)
├── MemoryValidator.h/.cpp   ← централізована валідація PlayerState/L2Char
├── ShadowLogger.h/.cpp      ← A/B Memory vs OCR → JSONL log
├── offset_scanner.h/.cpp    ← blindScan(), calibrate(), CE-style scan
├── offsets_config.h         ← ElmoreLab Kamael offsets (відкалібровано)
├── knownlist_reader.h/.cpp  ← region scan мобів, readName(), findMob()
├── world_state.h/.cpp       ← thread-safe WorldState + bgLoop
├── ProcessMemory.h          ← header-only process_vm_readv wrapper
├── l2_objects.h             ← L2Object, L2Character structs
├── game_state.h             ← GameState struct (snapshot per tick)
├── LinearQModel.h/.cpp      ← Q(s,a)=W^T*phi(s), IRLS+Huber, save/load
├── LearningWorker.h/.cpp    ← async IRLS thread (аналог GeodataWorker)
├── FeatureExtractor.h       ← 10 features GameState → Eigen::VectorXf
├── ExperienceBuffer.h       ← циклічний буфер Experience{s,a,r,s',done}
├── RewardCalculator.h       ← reward: kill/death/fail/buff/idle
├── Geodata.h/.cpp           ← L2J .geo loader + A*/JPS+ FindPath
├── geodata_worker.h/.cpp    ← async A* (Core 3)
├── navmesh_builder.h/.cpp   ← Recast/Detour runtime FindPath <1ms
├── navmesh_worker.h/.cpp    ← async Detour (Core 4)
├── navmesh_builder.cpp      ← Detour wrapper
├── vision_worker.h/.cpp     ← async DetectNPCs + DetectMinimap (Core 2)
├── Dashboard.h/.cpp         ← ncurses TUI (3 col + RL + Shadow)
├── Notify.h/.cpp            ← Telegram via fork+curl
├── Stats.h/.cpp             ← session stats + JSON log + /tmp live
├── RandomDelay.h            ← normal distribution delays (anti-detect)
├── FPS.h                    ← FPS moving average
├── Utils.h/.cpp             ← BitmapToImage, StringToKey, FuzzyMatch
├── build.sh                 ← компіляція (g++, без cmake)
├── rdga1bot.ini             ← активна конфігурація (не в git)
├── rdga1bot.example.ini     ← всі опції з коментарями
├── offsets.json             ← кеш знайдених KnownList offsets
├── weights.json             ← RL ваги (auto-saved)
│
├── qa/
│   ├── qa_monitor.py        ← Python daemon: IsolationForest + alerts
│   ├── anomaly_engine.py    ← rule-based + adaptive детектори
│   ├── alert_manager.py     ← notify-send + terminal bell
│   ├── dashboard_gen.py     ← HTML dashboard (Chart.js)
│   ├── screen_capture.py    ← scrot wrapper
│   └── mempalace_bridge.py  ← ChromaDB векторна БД bridge
│
├── scripts/
│   ├── navmesh_preview.py   ← 2D scatter + cluster analysis
│   └── navmesh_3d.py        ← Detour binary → 3D mesh render
│
└── third_party/
    └── eigen/               ← Eigen 3.4.0 header-only
```

### §3.2 · Main Loop (КАНОНІЧНИЙ — НЕ РЕСТРУКТУРУВАТИ)

```
while (!should_exit):
  # Input check (пріоритет)
  if ScrollLock/ESC → goto bot_exit

  # NavMesh: збір точок (кожну ітерацію, навіть hands busy)
  brain.TryRecordNavPoint()

  # Skip if hands busy
  if (!hands.IsReady()) → sleep(5ms), dashboard.Update(), continue

  # Window (кешований)
  if (!window_found) → Window::Find() → hands.SetGameWindow()

  # Capture
  capture.Clear()
  bitmap = capture.Grab(window_rect)  # XShm
  image = BitmapToImage(bitmap)
  eyes.Open(image)

  # Memory Reading
  if (mem_enabled && mem_reader.IsOpen())
    brain.SetMemPlayerState(mem_reader.ReadPlayer())

  # KnownList (async bg thread)
  if (kl_enabled && !brain.HasPlayerBase())
    → blindScan() в detached thread → kl_scan_result atomic
  if (kl_scan_result) → WorldState::startBackground()

  # Async Workers
  VisionWorker::SubmitFrame()  → TryGetResult() → brain.SetAsyncNPCs()
  GeodataWorker::TryGetResult() → brain.SetGeoPath()
  brain.GetPendingPathRequest() → geodata_worker.RequestPath()

  # Brain Tick (perception + BT + RL)
  brain.Process(debug)
  eyes.Close()

  # Dashboard (throttle 10 FPS)
  if (now - last_dashboard >= 100ms) → dashboard.Update()

  # Debug overlays (F12, PrintScreen, Space, F12)
```

### §3.3 · BotBehaviorTree — ПОТОЧНЕ ДЕРЕВО (~22 вузли)

```
Selector root
├── Sequence(condIsDead,    actDead)      — відродження (3 фази + grace 30s)
├── Sequence(condNeedsRest, actRest)      — пауза при низькому HP/MP
├── Sequence(condZoneViol,  actZone)      — повернення в зону фарму
├── Sequence(condNeedsBuff, actBuff)      — ALT+B буфінг (template matching)
├── Sequence(condLootPend,  actLoot)      — ESC+300ms після kill
├── Sequence(condHasTarget, actAttack)    — атака + kill detection
└── Selector (tgt_root, MR28)            — пошук нової цілі (7 вузлів)
      ├── actTgtInit        — hands_ready + ініціалізація + breadcrumb
      ├── actTgtDeadTarget  — мертвий таргет hp=0 → ESC/macro
      ├── actTgtMinimap     — ротація до моба на мінімапі
      ├── actTgtF2AndMacro  — F2 nexttarget + macro fallback + pokemon
      ├── actTgtNavigation  — stuck detection + breadcrumbs + memory nav
      ├── actTgtGeoPath     — NavMesh + geodata waypoints + WalkForward
      └── actTgtPatrol      — patrol + rotate + explore (завжди Running)
```

**Критичне правило BT init() — BFS порядок:**
1. addChild(root, всі seq/action) підряд
2. addChild(seq, їх нащадки)

Порушення → circular childStart → infinite loop у VM.

**thread_local BotBehaviorTree* s_self** — static Action/Condition через pointer (single-threaded — безпечно).

### §3.4 · BT Сигнатури (НІКОЛИ не змінювати)

```cpp
// tick() — незмінна сигнатура
std::string BotBehaviorTree::tick(GameState& gs);

// Condition
bool condXxx(GameState& gs);       // bool, читає тільки

// Action
BTStatus actXxx(GameState& gs);    // Success/Failure/Running

// m_children[] — НЕ переставляти під час виконання (ламає BTState)
```

### §3.5 · WorldState та GameState

```cpp
// GameState: знімок-значення на один тік, НЕ зберігати між тіками
struct GameState {
    Eyes& eyes; Hands& hands; const Config& cfg; Stats& stats;
    int hp, mp, cp; bool hp_valid;
    float player_x, player_y, player_z; bool coords_valid;
    std::optional<Eyes::Target> target; bool has_target;
    std::vector<Eyes::MinimapDot> minimap_dots;
    std::vector<L2Character> kl_mobs; int kl_alive_count; bool kl_mob_died;
    bool is_dead; bool in_grace; bool hands_ready;
    // callbacks: navigate_to_mob, is_blacklisted, blacklist_mob, ...
};

// WorldState: thread-safe з bgLoop (фоновий скан кожні 1000ms)
// Читання: auto mobs = world->mobs();  // copy під mutex
// Запис:   тільки через bgLoop() або world->setPlayerBase()
```

### §3.6 · Memory Safety (KnownList)

```cpp
// isValidPtr: Wine 32-bit user space
static bool isValidPtr(uintptr_t v) {
    return v > 0x10000 && v < 0xBFFF0000;
}

// Кожне process_vm_readv:
uint32_t ptr = rpm<uint32_t>(addr);
if (!isValidPtr(ptr)) continue;  // ОБОВ'ЯЗКОВО

// HP читання (MR43 — 2-hop via game_obj):
// render_node+0x58 → game_obj → +0x14 = HP (uint32, NOT float!)
// render_node+0x100 = interpolated X (НЕ HP!)
// render_node+0x180 = 0x80000000 (НЕ isDead!)

// blindScan: автономний, без координат, ~2-10s
// playerBaseCache: з offsets.json → Attempt 0 (без blindScan якщо XYZ валідні)
```

### §3.7 · RL Integration (опціонально)

```ini
[Learning]
Enabled = false           # default: вимкнено, НУЛЬ overhead
LearningRate = 0.1
DiscountFactor = 0.95
EpsilonStart = 1.0
EpsilonMin = 0.05
EpsilonDecay = 0.9995
HuberDelta = 1.345
WeightsFile = ./weights.json
FeatureLogInterval = 300  # [RL-F] рядки у лог кожні N тіків
```

```
RL потік за тік:
  rlPreTick(gs)     → extract phi(s), epsilon-greedy → m_rl_suggested_action
  BT tick           → виконує гілки, встановлює kill/death/buff/fail сигнали
  rlPostTick(gs)    → reward, push Experience, requestUpdate (async IRLS)

6 тактичних дій:
  TargetNearest=0, TargetWeighted=1, NavigateMemory=2,
  Patrol=3, RestNow=4, BuffNow=5

RL НЕ переставляє гілки BT — тільки hints через m_rl_suggested_action.
Softmax confidence ∈ (0,1] (fix від'ємних Q через maxCoeff).
```

### §3.8 · Конфіг INI Feature Flags (ВСІ Enabled=false за замовчуванням)

```ini
[BehaviorTree]      Enabled=true      # ОБОВ'ЯЗКОВО для роботи
[Learning]          Enabled=false     # Huber Q-Learning
[MemReader]         ShadowMode=false  # A/B Memory vs OCR log
[KnownList]         Enabled=false     # читання мобів з пам'яті
[NavMesh]           Enabled=false     # Detour pathfinding
[Geodata]           Enabled=false     # L2J .geo pathfinding
[Navigation]        Enabled=false     # memory XYZ навігація
[Threading]         Enabled=false     # VisionThread/GeodataThread
[Delays]            Enabled=false     # anti-detect варіативні затримки
[Breadcrumbs]       Enabled=false     # backtrack при застряганні
[Zone]              Enabled=false     # обмеження зони фарму
[WeightedTargeting] Enabled=false     # зважений вибір цілі
[Fuzzy]             Enabled=false     # Levenshtein mob name matching
```

---

## §4 · СТАН ПРОЄКТУ

### Завершені MR (станом на 2026-04-16)

| MR | Що зроблено |
|----|-------------|
| MR9-10 | Farm test: 133 kills/10min, 0 deaths |
| MR11-15 | ObjectiveManager → BotBehaviorTree |
| MR16-17 | HP fix ElmoreLab, JPS+, minimap throttle |
| MR18-19 | Breadcrumbs + NavMesh Recast/Detour |
| MR20a-c | BehaviorTree stackless VM + Farm BT |
| MR23-25 | RL: Eigen, LinearQModel, IRLS, LearningWorker |
| MR26 | MemoryValidator + blindScan(timeout) + ShadowLogger |
| MR27 | actTarget → 6 приватних tgtHandle* instance methods |
| MR28 | Target Selector піддерево з 7 BT вузлів |
| MR29 | RL активовано (Enabled=true), condNeedsRest overrides |
| MR30 | RL logs → Brain::Log() через LogFn callback |
| MR31 | condIsDead: `!inGrace()` (fix respawn memory lag) |
| MR32 | blindScan coordinate filter: `|X|<1000 && |Y|<1000` |
| MR33 | loadWeights() двопрохідна валідація |
| MR34 | RL повні overrides + softmax confidence |
| MR35 | playerBaseCache → Attempt 0 (без blindScan якщо валідний) |
| MR36 | Видалено Options.cpp/.h (legacy dead code) |
| MR37 | FeatureLogInterval: [RL-F] рядок кожні N тіків |
| MR38-39 | blindScan scanmem-style maps parser + pread |
| MR41 | CE reverse scan → forward scan; HP>0 filter |
| MR42 | Діагностика: render_node dump + game_obj pool scan |
| **MR43** | **HP FIX: render_node+0x58 → game_obj → +0x14 = HP (uint32!)** |

**Поточна фаза: MR44+**

### Стан систем

| Система | Файл | Статус |
|---------|------|--------|
| Vision (OpenCV+XShm) | Eyes.h | ✅ |
| Memory (process_vm) | MemReader.h | ✅ |
| KnownList scanner | knownlist_reader.h | ✅ |
| MemoryValidator | MemoryValidator.h | ✅ |
| ShadowLogger A/B | ShadowLogger.h | ✅ |
| BehaviorTree VM | BehaviorTree.h | ✅ |
| Farm BT + Target subtree | BotBehaviorTree.h | ✅ |
| RL Q-Learning (opt) | LinearQModel.h | ✅ |
| XTest input | Hands.h | ✅ |
| RandomDelay anti-detect | RandomDelay.h | ✅ |
| Config INI parser | Config.h | ✅ |
| Dashboard ncurses TUI | Dashboard.h | ✅ |
| Geodata A*/JPS+ | Geodata.h | 🟡 opt |
| NavMesh Detour | navmesh_builder.h | 🟡 opt |
| Telegram Notify | Notify.h | ✅ |
| QA Monitor (Python) | qa/qa_monitor.py | 🟡 WIP |
| MemPalace ChromaDB | qa/mempalace_bridge.py | 🟡 opt |

---

## §5 · ТЕХНІЧНІ БОРГИ

### 🔴 КРИТИЧНІ

**[БОРГ-1] KnownList offsets не узгоджені (Kamael клієнт)**
- Файл: `offsets_config.h`, `knownlist_reader.h`
- Проблема: OFF_CHAR_HP=0x100 читає interpolated X (НЕ HP). OFF_CHAR_IS_DEAD=0x180 = 0x80000000 (НЕ isDead). Виправлено в MR43 через game_obj 2-hop.
- Стан: `readMobs()` legacy шлях досі використовує broken offsets. `readMobsRegionScan()` — виправлено.
- Рішення: Уніфікувати всі шляхи читання через 2-hop OFF_GAME_OBJ_PTR.

**[БОРГ-2] KnownList isValidPtr guard не скрізь**
- Файл: `knownlist_reader.cpp`
- Проблема: Деякі старі виклики process_vm_readv без isValidPtr перевірки.
- Ризик: segfault при invalid pointer.
- Рішення: Аудит всіх rpm<>() викликів, додати guard.

### 🟡 НЕКРИТИЧНІ

**[БОРГ-3] RandomDelay: один глобальний distribution (не per-action)**
- Файл: `RandomDelay.h`, `Config.h`
- Проблема: однаковий патерн для attack/move/buff → детект.
- Рішення: `Config::delay_per_action` → `RandomDelay::get(action_type)`.

**[БОРГ-4] Dashboard: ncurses resize (SIGWINCH) — частковий**
- Файл: `Dashboard.cpp`

**[БОРГ-5] RL: FeatureExtractor kl_alive_count не нормалізований для великих зон**
- Файл: `FeatureExtractor.h` (f[4] = clamp(kl_alive_count/10, 0,1))

**[БОРГ-6] WorldState bgLoop: kScanIntervalMs=1000 → max 1s kill detection lag**
- Файл: `world_state.cpp`

---

## §6 · ДОРОЖНЯ КАРТА

**Поточна фаза: MR44 · KnownList Stability + Live Farm**

```
[ ] kl_alive_count > 0 верифікація у live farm
[ ] kill detection спостереження + RL features logging
[ ] [БОРГ-1] Уніфікувати HP читання (game_obj 2-hop скрізь)
[ ] [БОРГ-2] isValidPtr audit у KnownListReader
[ ] [БОРГ-3] RandomDelay per-action config
```

**MR45 · Stability**
```
[ ] Memory leak audit (valgrind --leak-check=full)
[ ] KnownList stress test: 2000 objects, 10 min run
[ ] Config validation: schema + hot-reload rollback
[ ] WorldState bgLoop: скорочення scan interval до 300ms
```

**MR46 · RL Integration**
```
[ ] RL enable на тестовому сервері (Enabled=true в .ini)
[ ] weights.json auto-backup перед оновленням
[ ] Dashboard: RL epsilon live display
[ ] FeatureExtractor: нормалізація kl_alive_count для зон з > 50 мобів
```

**MR47 · QA + NavMesh**
```
[ ] qa_monitor.py IsolationForest integration test
[ ] NavMesh: нова збірка для поточної зони фарму (LoA/Tower)
[ ] NavMesh 3D візуалізація (navmesh_3d.py)
[ ] MemPalace: baseline kill rate з 22 сесій (~57799 kills)
```

---

## §7 · ОПЕРАЦІЙНИЙ ПРОТОКОЛ

### §7.1 · Формат запиту (ОБОВ'ЯЗКОВИЙ)

```
PROJECT: rdga1bot | PHASE: MR44 | TASK: [одне речення]
FILES: [макс. 3-4 файли що змінюються]
CONSTRAINT: [що НЕ можна / KnownList safety / no root / anti-detect]
OUTPUT: REPLACE blocks only. No prose.
```

### §7.2 · Формат відповіді

```cpp
// FILE: BotBehaviorTree.cpp
// REPLACE START: actAttack
BTStatus BotBehaviorTree::actAttack(GameState& gs) {
    // ... код ...
}
// REPLACE END
```

**НІКОЛИ не приймати:**
- Повний файл, якщо змінено < 5 функцій
- Код без `// REPLACE START/END`
- Нові header без `#pragma once`
- Зовнішні залежності без узгодження
- KnownList читання без isValidPtr guard
- W/S/A/D замість стрілок для руху
- `UseForKillDetect=true`

### §7.3 · Правило "Нульового розмішу"

Код ЗА МЕЖАМИ REPLACE блоку повинен бути **ІДЕНТИЧНИЙ** оригіналу. Заборонено: змінювати відступи, коментарі, порожні рядки поза блоком.

### §7.4 · Бюджет токенів

| Задача | Вхід ліміт | Стратегія |
|--------|-----------|-----------|
| Баг-фікс | ~400 | Тільки функція + помилка |
| Мала фіча | ~800 | Тільки .h файли + stub impl |
| Нова система | ~1500 | Phase 1: header → Phase 2: impl |
| Рефакторинг | ~600 | Тільки diff + тест |
| > 4 файлів | ~1000 | Спочатку архітектурний план |

**Правило 3-ітерацій**: Не вирішено за 3 запити → STOP, зроби stub, повернись пізніше.

### §7.5 · Fail-Safe Protocol (rdga1bot-specific)

```
Порушує memory safety         → // ERROR: VIOLATES CONSTRAINT [process_vm_readv rules]
Файл ∉ архітектури            → // ERROR: FILE NOT IN CANONICAL STRUCTURE: [name]
KnownList без валідації        → // ERROR: KnownList read requires isValidPtr guard
XTest без Delay                → // WARNING: Add RandomDelay if anti-detect needed
W/A/S/D для руху              → // ERROR: Use arrow keys only (chat opens with WASD)
UseForKillDetect=true          → // ERROR: PERMANENT FALSE — causes fake kills
RL без Enabled=false default   → // WARNING: RL overhead, confirm Enabled=false default
CMakeLists замість build.sh    → // ERROR: Project uses build.sh only
```

→ НЕ генеруй код, НЕ вигадуй offsets, НЕ пропонуй Cheat Engine.

### §7.6 · Локальний Fallback

- **ollama (qwen2.5-coder:7b)** — boilerplate, форматування, прості утиліти
- **Claude Code** — складна BT логіка, архітектура, memory safety, anti-detect, RL

---

## §8 · СТАНДАРТИ КОДУ

### §8.1 · Новий компонент (POD)

```cpp
#pragma once
#include <cstdint>
// ТІЛЬКИ POD або trivially copyable
// БЕЗ методів з бізнес-логікою (тільки static factory)
struct L2NewObject {
    uintptr_t memPtr = 0;
    float x = 0.f, y = 0.f, z = 0.f;
    int objectID = 0;
    bool valid() const { return memPtr != 0; }
};
```

### §8.2 · BT Action/Condition

```cpp
// ACTION — повертає Running/Success/Failure
BTStatus BotBehaviorTree::actNew(GameState& gs) {
    if (!s_self) return BTStatus::Failure;
    auto& self = *s_self;
    self.m_active_branch = "NewBranch";  // для Dashboard

    if (!gs.hands_ready) return BTStatus::Running;
    if (!gs.hp_valid || gs.is_dead) return BTStatus::Failure;

    // ... логіка ...
    gs.hands.Send(200);
    return BTStatus::Running;
}

// CONDITION — читає тільки
bool BotBehaviorTree::condNew(GameState& gs) {
    if (!s_self || gs.is_dead) return false;
    return /* умова */;
}
```

### §8.3 · Debug/Safety (замість assert)

```cpp
if (!isValidPtr((uintptr_t)ptr)) {
    std::cerr << "[Mem] invalid ptr at " << __FILE__ << ":" << __LINE__ << "\n";
    return default_value;
}
if (obj_id >= KL_MAX_OBJECTS) {
    std::cerr << "[KL] id " << obj_id << " OOB\n";
    return false;
}
if (!gs.hp_valid || gs.is_dead) return BTStatus::Failure;
```

### §8.4 · Нова фіча — чек-лист

```
[ ] Thread-safe: snapshot pattern або mutex?
[ ] Memory safety: isValidPtr guard для кожного rpm<>()?
[ ] Anti-detect: RandomDelay для XTest actions?
[ ] Config: INI секція + validation + feature flag (Enabled=false)?
[ ] Dashboard TUI output?
[ ] Немає malloc/new у vision/BT hot-path?
[ ] RL: Enabled=false default, нуль overhead при вимкненні?
[ ] Рух: тільки стрілки (не W/A/S/D)?
[ ] Kill detection: не UseForKillDetect?
[ ] Auto-loot: тільки ESC+300ms (server-side на ElmoreLab)?
```

**Правило "Видимість = Контроль"**: Кожна нова система ПОВИННА мати вивід у Dashboard TUI до того, як вважається завершеною.

---

## §9 · DASHBOARD TUI СПЕЦИФІКАЦІЯ

### §9.1 · ncurses ініціалізація

```cpp
// Dashboard.cpp
initscr(); cbreak(); noecho();
keypad(stdscr, TRUE);
start_color(); use_default_colors();
timeout(1);  // non-blocking getch()
// 3 колонки: [HP/MP/CP + mob] [K/D + memory] [RL status]
// Resize: SIGWINCH → RecreateWindows()
```

### §9.2 · Кольори + layout

```
COLOR_PAIR(HP_BAR)  = RED    → HP bar
COLOR_PAIR(MP_BAR)  = BLUE   → MP bar
COLOR_PAIR(CP_BAR)  = YELLOW → CP bar
COLOR_PAIR(ATTACK)  = GREEN  → Attack state
COLOR_PAIR(TARGET)  = YELLOW → Targeting state
COLOR_PAIR(DEAD)    = RED    → Dead state
COLOR_PAIR(BUFF)    = MAGENTA → Buff state
COLOR_PAIR(LOOT)    = CYAN   → Loot state
COLOR_PAIR(TITLE)   = CYAN   → Header

Layout рядки:
[0] Header: "rdga1bot" + state + FPS
[1] Separator
[2] HP bar | Mob HP bar | [MEM]/[OPENCV]/[HYBRID]
[3] MP bar | K/D stats  | X/Y coordinates
[4] CP bar | Potions    | Mobs: N dist: D
[5] RL:    | Shadow:    | (якщо активні)
[log area] Logs (scroll, max 200)
[footer]   Key hints
```

### §9.3 · Toggle клавіші

```
P    = toggle pause
S    = settings overlay (hot-reload Config)
R / Space = reset HP/MP/CP bars detection
F12  = HSV calibration → calibrate_*.png
ScrollLock = зупинка бота (global, без фокусу)
ESC  = зупинка (з grace period 500ms після старту)
Q    = зупинка (у TUI)
```

---

## §10 · ШАБЛОНИ ЗАПИТІВ

### HP читання уніфікація (БОРГ-1)

```
PROJECT: rdga1bot | PHASE: MR44 | TASK: Уніфікувати HP читання через game_obj 2-hop
FILES: knownlist_reader.cpp, knownlist_reader.h, offsets_config.h
CONSTRAINT: readMobs() та readAllAsChars() мають використовувати той самий
OFF_GAME_OBJ_PTR/HP шлях що і readMobsRegionScan(). НЕ змінювати логіку скану.
НЕ змінювати L2Character struct. isValidPtr перед кожним rpm<>().
OUTPUT: REPLACE blocks only. No prose.
```

### isValidPtr audit (БОРГ-2)

```
PROJECT: rdga1bot | PHASE: MR44 | TASK: isValidPtr guard у всіх точках KnownListReader
FILES: knownlist_reader.cpp
CONSTRAINT: Додати if (!isValidPtr(ptr)) continue; перед кожним rpm<uint32_t>().
НЕ змінювати логіку читання, тільки safety guards.
OUTPUT: REPLACE blocks only. No prose.
```

### RandomDelay per-action (БОРГ-3)

```
PROJECT: rdga1bot | PHASE: MR44 | TASK: RandomDelay з config per action type
FILES: RandomDelay.h, Config.h, BotBehaviorTree.h
CONSTRAINT: Додати struct DelayConfig per-action (attack/rotate/walk/potion).
Зберегти backward compat: default {500ms, 75ms} для attack.
Оновити BotBehaviorTree::tgtHandlePatrolAndRotate → RandMs() з правильним RD.
OUTPUT: REPLACE blocks only. No prose.
```

### Нова BT гілка

```
PROJECT: rdga1bot | PHASE: MR44 | TASK: [опис гілки]
FILES: BotBehaviorTree.h, BotBehaviorTree.cpp
CONSTRAINT: Зберегти BFS порядок addChild() в init(). Нова гілка = Sequence(cond, act).
actNew: s_self guard + m_active_branch + hands_ready check.
condNew: is_dead + in_grace guards. НЕ змінювати tick() сигнатуру.
OUTPUT: REPLACE blocks only. No prose.
```

---

## §11 · ЗАХИЩЕНІ РІШЕННЯ (НЕ ВИПРАВЛЯТИ)

```
[1] process_vm_readv без ptrace — легалізований метод без root на Linux
[2] WorldState::mutex_ для bg thread — необхідний для thread-safe snapshot
[3] Власний INI-парсер (strstr) — НЕ пропонувати boost чи yaml-cpp
[4] 24-byte BehaviorTree ноди — cache-line aligned, static_assert підтверджено
[5] Single-thread BT core — детермінізм > паралелізм, async workers для важких ops
[6] ncurses замість ImGui — нуль графічних залеж., SSH/headless сумісність
[7] KnownList blindScan — автономний, без координат, fallback з offsets.json
[8] qa_monitor.py на Python — ізольований daemon, НЕ імпортувати з C++
[9] build.sh без cmake — простота + прозорість флагів компіляції
[10] Eigen 3.4.0 header-only — тільки для RL, нуль overhead при Enabled=false
[11] fork+curl для Telegram — без libcurl, без залежностей
[12] Auto-loot = ESC+300ms — server-side лут на ElmoreLab (без pickup key)
[13] рух тільки стрілками — W/A/S/D відкривають чат в L2
[14] softmax confidence для RL — fix від'ємних Q (maxCoeff давав confidence=0)
[15] playerBaseCache Attempt 0 — уникає blindScan якщо XYZ з offsets.json валідні
```

---

## §12 · PERFORMANCE BUDGET

```
[Vision] frame capture:  ≤10ms  (XShm, lazy HSV)
[Vision] NPC detect:     ≤15ms  (async VisionWorker Core 2)
[Memory] KnownList scan: ≤50ms  (timeout guard, bg thread)
[BT]     tick:           ≤5µs   (stackless VM, ~22 вузли, avg ~2µs)
[RL]     preTick:        ≤1ms   (Eigen multiply, sync)
[RL]     IRLS update:    async  (LearningWorker, окремий thread)
[Nav]    Detour FindPath: <1ms  (runtime only, без Recast)
[Nav]    A* FindPath:    ≤50ms  (GeodataWorker Core 3)
[UI]     Dashboard:      10 FPS (100ms throttle, non-blocking)
[RAM]    загальне:       ≤2 GB  (OpenCV buffers + KnownList + NavMesh)
[FPS]    target:         30-60  (залежить від NPC detection)

Моніторинг у Brain::Process():
  if (tick_ms > 50.0)
    Log("[PERF] Повільний тік: " + to_string(tick_ms) + "мс", Warning);
```

---

## §13 · ВІДОМІ PITFALLS

```
• XShm: перевіряти MIT-SHM extension перед ініціалізацією
• process_vm_readv: partial read possible → перевіряти return value == sizeof
• KnownList: offsets змінюються з патчем гри → blindScan fallback
• XTest: фокус вікна → EnsureGameFocused() один раз на серію подій (кешовано)
• ncurses: resize (SIGWINCH) → RecreateWindows() патерн
• RL weights.json: encoding UTF-8, двопрохідна валідація при завантаженні
• Telegram curl: fork + waitpid для уникнення zombie processes
• Config hot-reload: через 's' у TUI → brain.ReloadConfig()
• Wine: XTest через XTestFakeKeyEvent, НЕ xdotool --window
• Wine: process_vm_readv PID = wine subprocess PID (findL2Pid() через /proc)
• Game patch: KnownList offsets змінюються → blindScan() + calibrate() ПЕРЕД сесією
• PlayerBase validity: кожні 30s перевіряти X/Y > 500 (OR умова, не AND — Y=0 в деяких зонах)
• MR43: render_node+0x100 = interpolated X (НЕ HP!), +0x180 = 0x80000000 (НЕ isDead!)
• condIsDead: !inGrace() guard (fix respawn memory lag — 3 death episodes з однієї смерті)
• blindScan XY filter: |X|<1000 AND |Y|<1000 — fix для ToI/LoA (X<30000)
• actRest RL override: confidence > 0.5 (не maxCoeff — від'ємні Q не спрацьовували)
• NavMesh coords: L2_X→Recast_X, L2_Z(висота)→Recast_Y(up), L2_Y→Recast_Z
```

---

## §14 · ШПАРГАЛКА

```
❓ "OpenCV замість власного vision?"        → Так, lazy HSV конвертація
❓ "std::thread для BT?"                    → Ні, single-thread + async workers
❓ "Новий buff у Config?"                   → [Buffs] → hot-reload через 's'
❓ "KnownList offset змінився?"             → blindScan() + calibrate()
❓ "RL overhead при Enabled=false?"         → Нуль (nullptr check)
❓ "XTest не працює?"                       → $DISPLAY, xhost +local:, вікно фокус
❓ "Dashboard не оновлюється?"              → timeout(1), DASHBOARD_INTERVAL 100ms
❓ "process_vm_readv permission denied?"    → перевірити /proc/[pid]/mem perms
❓ "Рух персонажа не працює?"              → тільки стрілки, НЕ WASD!
❓ "Fake kills?"                            → UseForKillDetect=false ЗАВЖДИ
❓ "HP моба завжди 0?"                     → MR43 2-hop: rpm(base+0x58)→rpm(go+0x14)
❓ "PlayerBase не знайдено?"               → стань поруч з мобами, жди 30s blindScan
❓ "isAlive() false для живих?"            → перевір OFF_GAME_OBJ_PTR/HP в offsets.json
❓ "Auto-loot не підбирає?"                → server-side: тільки ESC+300ms (ElmoreLab)
❓ "NavMesh не завантажується?"            → build_navmesh спочатку, потім Enabled=true
❓ "BT infinite loop?"                     → перевір BFS порядок addChild() в init()
```

---

## §15 · АРХІТЕКТУРНІ РІШЕННЯ (ADR)

**Чому stackless BT?**
Farm bot годинами, потрібен детермінізм + серіалізація стану. 24-byte ноди (cache-friendly). MAX_NODES=256 достатньо для farm логіки. Відхилено: рекурсивний BT (stack overflow), FSM (>15 станів = хаос).

**Чому process_vm_readv?**
ptrace вимагає root або CAP_SYS_PTRACE → детект анти-чітом. process_vm_readv працює з правами користувача якщо /proc/[pid]/mem доступний. Відхилено: LD_PRELOAD (детект), kernel module (overkill).

**Чому INI замість JSON для config?**
INI простіший для ручного редагування + ncurses TUI editor. JSON залишено тільки для weights.json (RL матриці). Відхилено: YAML (зайня залеж.), TOML (менш поширений у C++).

**Чому ncurses?**
rdga1bot запускається в headless/SSH. ncurses: нуль графічних залежностей. Overhead: <0.1ms. Відхилено: ImGui (X11 dependency, 2ms overhead).

**Чому KnownList MAX_OBJECTS=2000?**
L2 Kamael: ~500-1500 мобів у радіусі агро. 2000 = headroom. 2000×64B = 128KB (прийнятно). Відхилено: std::vector (heap alloc у hot-path), 4096 (overkill).

**Чому 2-hop HP читання (MR43)?**
render_node+0x58 → game_obj → +0x14 = HP (uint32). render_node+0x100 = interpolated X (не HP!). Підтверджено калібровкою: LiveHP = uint32, NOT float. isAlive() тепер правильно для живих мобів.

**Чому playerBaseCache Attempt 0?**
blindScan (~2-10s) → неприйнятно кожні 30s. offsets.json зберігає валідну адресу між сесіями. XYZ перевірка при завантаженні (|X|>100 OR |Y|>100). Відхилено: завжди blindScan (latency), жорстке кодування (зміни між сесіями).

---

## §16 · ПРОТОКОЛ ОНОВЛЕННЯ ЦЬОГО ФАЙЛУ

**Коли**: завершено MR## | змінено стек | нова система | проблема в протоколі.

**Чек-лист:**
```
[ ] VERSION vX.Y → vX.Y+1
[ ] CHANGELOG оновлено (§17)
[ ] Всі § відповідають актуальним файлам проєкту
[ ] Шаблони §10 протестовано на реальному запиті
[ ] БОРГи оновлено: закриті → видалено, нові → додано
[ ] CLAUDE.md.bak збережено перед перезаписом
```

---

## §17 · CHANGELOG

```
v1.0 — 2026-01-22 INITIAL (адаптація monkey_dust CLAUDE_v4.md)
  • Контекст: L2 bot, OpenCV, XShm, process_vm_readv, BehaviorTree
  • Safety: KnownList isValidPtr, WorldState snapshot
  • Anti-detect: RandomDelay
  • RL: Enabled=false, weights.json validation

v2.0 — 2026-04-16 ADAPTED TO ACTUAL CODEBASE
  • §0: Точний контекст (CachyOS, ElmoreLab Kamael, ManyaTheBond, Treasure Hunter)
  • §3.1: Реальна файлова структура (замість ідеальної з v1.0)
  • §3.3: BotBehaviorTree дерево (~22 вузли, MR27-28 Target subtree)
  • §3.6: MR43 HP fix (2-hop game_obj, uint32 не float)
  • §4: Завершені MR9-MR43, поточна фаза MR44
  • §5: Борги оновлено (БОРГ-1: HP читання, не isValidPtr guard як раніше)
  • §11: Захищені рішення розширено (15 пунктів, +MR43/playerBaseCache)
  • §13: Додано pitfalls специфічні для MR31-MR43
  • §14: Шпаргалка розширена (HP/autoload/NavMesh/BT pitfalls)
  • §15: ADR розширено (2-hop HP, playerBaseCache)
  • Видалено: §12 GoogleTest (не використовується в поточному проекті)
  • Виправлено: build.sh НЕ cmake; auto-loot = ESC+300ms (не pickup key)
```

---

```
# ═══════════════════════════════════════════════════════════════════
# rdga1bot CLAUDE.md v2.0 — КІНЕЦЬ КОНСТИТУЦІЇ
# Зберегти як: ~/l2bot/rdga1bot/CLAUDE.md
# НЕ видаляти · НЕ скорочувати · ОНОВЛЮВАТИ згідно §17
# ═══════════════════════════════════════════════════════════════════
```
