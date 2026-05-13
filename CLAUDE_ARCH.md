# rdga1bot — Архітектура

## Планувальник
- **BotBehaviorTree** — єдиний активний планувальник (ObjectiveManager видалено в MR20c)
- Stackless BT VM: ~9KB static, 0 heap alloc, ~2µs/тік
- `[BehaviorTree] Enabled=true` — обов'язково для роботи

## BotBehaviorTree дерево
```
Selector root
├── Sequence(condIsDead,   actDead)    — відродження (3 фази: Enter→Enter→grace 30с)
├── Sequence(condNeedsRest,actRest)    — пауза при низькому HP/MP
├── Sequence(condZoneViol, actZone)    — повернення в зону фарму
├── Sequence(condNeedsBuff,actBuff)    — ALT+B буфінг (template matching)
├── Sequence(condLootPend, actLoot)    — ESC+300мс після kill
├── Sequence(condHasTarget,actAttack)  — атака + kill detection
└── Selector (tgt_root, MR28)          — пошук нової цілі (7 вузлів)
      ├── actTgtInit        — hands_ready + ініціалізація + breadcrumb
      ├── actTgtDeadTarget  — мертвий таргет hp=0 → ESC/macro
      ├── actTgtMinimap     — ротація до моба на мінімапі
      ├── actTgtF2AndMacro  — F2 nexttarget + macro fallback + pokemon
      ├── actTgtNavigation  — stuck detection + breadcrumbs + memory nav
      ├── actTgtGeoPath     — NavMesh + geodata waypoints + WalkForward
      └── actTgtPatrol      — patrol + rotate + explore (завжди Running)
```
`thread_local BotBehaviorTree* s_self` — static Action/Condition функції звертаються через pointer (single-threaded — безпечно).

## BT init() — КРИТИЧНО: BFS порядок
1. addChild(root, всі seq/action) підряд
2. addChild(seq, їх нащадки)

Порушення → circular childStart → infinite loop у VM.

---

## CATHODE-inspired стратегічний шар (v1.6)

### Blackboard (`src/Blackboard.h/.cpp`)
Lock-free shared state між Director (500ms) і BT (кожен тік ~100ms).
Три типізованих flat-масиви на `std::atomic`:
- `m_floats[12]` — `PLAYER_HP_PCT`, `KILLS_PER_MIN`, `MOB_DENSITY`, `ZONE_CX/CY/RADIUS`, `MOOD_INTENSITY` …
- `m_ints[6]`   — `ALIVE_MOB_COUNT`, `UNREACHABLE_STREAK`, `CURRENT_MOOD`, `CURRENT_DIRECTIVE` …
- `m_bools`     — 10 бітів в `atomic<uint64_t>`: `HAVE_TARGET`, `CLOSE_THREAT`, `FLEE_ACTIVE`, `ZONE_VALID` …

Director writes: `memory_order_release`. BT reads: `memory_order_relaxed` (x86 ABI safe).
`syncFromGameState(gs)` — Brain оновлює BT-owned слоти кожен тік.

### DirectorSystem (`src/DirectorSystem.h/.cpp`)
Стратегічний аналіз кожні 500ms (`ANALYSIS_MS`):
- `updateKillRate()` — rolling 60s KPM → `BB::Float::KILLS_PER_MIN`
- `updateDeathStreak()` — consecutive deaths → `BB::Int::DEAD_STREAK`
- `updateZone()` — ініціалізує ZoneRecord (cx/cy/radius) при першій валідній XYZ → `BB::Bool::ZONE_VALID`
- `updateMood()` → MoodManager → `BB::Int::CURRENT_MOOD` + `BB::Float::MOOD_INTENSITY`
- `publishDirective()` → `BB::Int::CURRENT_DIRECTIVE` + `BB::Bool::FLEE_ACTIVE`

`StrategicDirective`: `FARM_HERE / REPOSITION / REST_FIRST / RETURN_TO_ZONE / FLEE`

Single-threaded (Brain main thread); аналіз <1ms → negligible в 100ms бюджеті.

### BotMood (`src/BotMood.h`)
`NEUTRAL / AGGRESSIVE / CAUTIOUS / FLEE / SUSPICIOUS`

MoodManager — hysteresis guard: кандидат має бути stable 3 eval перед перемиканням.
FLEE — миттєво (безпека > плавність).

Reward shaping multipliers:
| Mood | kill_scale | death_scale | rest_bonus | move_bonus |
|------|-----------|-------------|------------|------------|
| AGGRESSIVE | 1.25 | 0.80 | 0.00 | 0.00 |
| CAUTIOUS | 0.80 | 1.50 | 0.20 | 0.00 |
| FLEE | 0.50 | 2.00 | 0.00 | 0.30 |
| SUSPICIOUS | 1.50 | 1.00 | 0.00 | 0.00 |

### SenseSystem (`src/SenseSystem.h`)
Tiered perception: `MINIMAL → STANDARD → HEIGHTENED → FULL`
- MINIMAL: `hp_valid`
- STANDARD: + minimap або has_target
- HEIGHTENED: + `coords_valid` (MemReader XYZ)
- FULL: + KnownList (`kl_mobs` або `kl_alive_count > 0`)

`evaluateSenseSet(gs)` — inline, called per-tick.

---

## RL інтеграція (`[Learning] Enabled=true`)

### Потік даних за тік:
```
rlPreTick(gs)          — extract phi(s), epsilon-greedy → m_rl_suggested_action
BT tick (незмінений)   — виконує гілки, встановлює RL сигнали (kill/death/buff/fail)
rlPostTick(gs)         — reward (mood-aware), push Experience, requestUpdate (async IRLS)
```

### RL компоненти:
- `LinearQModel`     — Q(s,a) = W[:,a]^T * phi(s), 10 features × 6 actions
- `ExperienceBuffer` — циклічний буфер N=1000, без alloc після init
- `LearningWorker`   — async IRLS thread, Huber loss
- `FeatureExtractor` — 10 ознак: hp, mp, has_target, target_hp, kl_alive, minimap_dots, secs_kill, secs_buff, is_dead, in_grace
- `RewardCalculator` — +kill(1.0), -death(5.0), -targeting_fail(0.01), +buff(0.1), idle(-0.001); масштабується на MoodRewardScale

### 6 тактичних дій RL:
```
TargetNearest=0, TargetWeighted=1, NavigateMemory=2,
Patrol=3, RestNow=4, BuffNow=5
```

---

## Build система (CMakePresets.json)

| Preset | Папка | Flags | Час |
|--------|-------|-------|-----|
| `debug-fast` | `build_debug/` | `-O0 -g -D_GLIBCXX_ASSERTIONS` | ~2 хв |
| `release` | `build/` | `-O2` | ~5 хв |
| `qa-debug` | `build_qa/` | ASan+UBSan + `-DRDGA1BOT_ALLOC_STATS=1` | ~15 хв |

```bash
cmake --preset debug-fast && ninja -C build_debug   # розробка
cmake --preset qa-debug   && ninja -C build_qa      # pre-merge sanity
```

**AllocStats** (`RDGA1BOT_ALLOC_STATS=1`): `operator new/delete` overrides з `atomic<uint64_t>` лічильниками. Логує `[AllocStats] allocs=N total_kb=M` кожні 600 тіків (~60с). Використовувати тільки в `qa-debug` — overhead ~5% через атомарні операції.

---

## GameState
Snapshot per tick. НЕ зберігати між тіками.

Ключові поля v1.6:
- `npcs` — `std::vector<Eyes::NPC>` — кешується раз на тік у `Brain::updateGameState()` (P1); BT вузли читають звідси, НЕ викликають `DetectNPCs()` напряму
- `blackboard` — `Blackboard*` — nullable; BT вузли читають mood/directive/flee_active

## WorldState — thread-safe агрегатор
`std::shared_mutex m_mutex` (v1.6 — P2 upgrade від `std::mutex`):
- **Shared lock** (читачі): `mobs()`, `copyMobsTo()`, `items()`, `target()`, `aliveCount()`, `hasValidTarget()`, `targetIsDead()`, `hasLootNearby()`, `refreshPlayerXYZ()`
- **Exclusive lock** (записувач): bgLoop scan/non-scan, `update()`, `setTarget()`, `clearTarget()`, `startBackground()`, `setPlayerBase()`

`copyMobsTo(std::vector<L2Character>& out)` (P3): копіює `m_mobs` прямо в `out` під shared lock — без тимчасового вектора.

## Мультипоточність
```
Main thread (Core 1): Brain tick, BT, DirectorSystem (~100ms)
Core 2: VisionWorker  — async DetectNPCs + DetectMinimap
Core 3: GeodataWorker — async A* FindPath
Core 4: NavMeshWorker — async Detour FindPath
Core -: LearningWorker— async IRLS batch update
Core -: WorldState bgLoop — KnownList scan кожну 1с
```

**UAF fix (P6)**: `Notify::~Notify()` spin-waits на `m_pending==0` (до 10с); `Input::~Input()` spin-waits на `m_threads==0` (до 2с). Threads захоплюють `this` — треба гарантувати, що об'єкт живий до їх завершення.

## Ключові файли
```
src/Brain.cpp                — координатор: сприйняття + потіони + dispatch
src/BotBehaviorTree.h/.cpp   — Farm BT ctor/init/tick/reset/conditions
src/BehaviorTree.h/.cpp      — Stackless VM (BTNode 24B, BTState 8B)
src/game_state.h             — GameState struct (npcs + blackboard ptr)
src/Blackboard.h/.cpp        — lock-free BB (atomic float/int/bool)
src/DirectorSystem.h/.cpp    — стратегічний AI, ZoneRecord, MoodManager
src/BotMood.h                — enum BotMood + MoodManager + reward scales
src/SenseSystem.h            — SenseSet evaluator (inline)
src/world_state.h/.cpp       — shared_mutex WorldState + copyMobsTo
src/LinearQModel.h/.cpp      — Q(s,a), IRLS, save/load JSON
src/LearningWorker.h/.cpp    — async IRLS thread
src/FeatureExtractor.h       — phi(s): GameState → Eigen::VectorXf (10 dim)
src/ExperienceBuffer.h       — Experience{s,a,r,s',done} circular buffer
src/RewardCalculator.h       — reward function + MoodRewardScale
src/MemoryValidator.h/.cpp   — централізована валідація PlayerState/L2Character
src/ShadowLogger.h/.cpp      — A/B Memory vs OCR → JSONL
src/AllocStats.h/.cpp        — operator new/delete counters (qa-debug only)
src/Intercept.h              — cross-platform input interface
src/Intercept_Linux.cpp      — XSendEvent hybrid backend
src/platform.h               — pid_t/ssize_t (Linux + Windows)
src/ProcessMemory.h          — process_vm_readv (Linux) / ReadProcessMemory (Windows)
src/Notify.h/.cpp            — Telegram: Send() async thread + ~Notify() join
src/Input.h/.cpp             — event queue + Send() async thread + ~Input() join
third_party/eigen/           — Eigen 3.4.0 header-only
.github/workflows/ci.yml     — CI: ubuntu-22.04, debug-fast + run-san
```

## Config INI секції (feature flags)
```ini
[BehaviorTree]  Enabled=true     ← обов'язково
[Learning]      Enabled=true     ← Huber Q-Learning (default ON з MR29)
[MemReader]     ShadowMode=false ← A/B Memory vs OCR лог
[KnownList]     Enabled=false    ← читання мобів з пам'яті
[NavMesh]       Enabled=false    ← Detour pathfinding
[Geodata]       Enabled=false    ← L2J .geo pathfinding
[Navigation]    Enabled=false    ← memory XYZ навігація
[Threading]     VisionThread=false / GeodataThread=false
[Delays]        Enabled=false    ← антидетект варіативні затримки
[Breadcrumbs]   Enabled=false
[Zone]          Enabled=false
[WeightedTargeting] Enabled=false
[Fuzzy]         Enabled=false
```
