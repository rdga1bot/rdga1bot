# rdga1bot — Контекст для Claude Code

## Середовище
- OS: Arch Linux (CachyOS), X11, користувач rdga1
- Гра: Lineage 2, ElmoreLab (Kamael/Lionna), персонаж ManyaTheBond (Treasure Hunter/Dagger)
- Клієнт: Flatpak Lutris + GE-Proton/Wine
- Папка: /home/rdga1/rdga1prj/l2net/
- Build: `cmake --preset debug-fast && ninja -C build_debug` (або `bash build.sh`)

## Поточний стан (2026-05-13)

- **BotBehaviorTree** — єдиний планувальник. RL: `[Learning] Enabled=true` за замовчуванням.
- **MR80** — HP читання з пам'яті ПІДТВЕРДЖЕНО. Два механізми (пріоритет: hp_abs > anchor > game_obj):
  - **Якір DSETUP.dll**: `*(0x1003F27C) - 0x3DC8 = struct_base` → `+0=max_hp`, `+8=cur_hp`
    - Адреса `0x1003F27C` (268694140 dec) стабільна впродовж сесії (ptr=0x248c8b00 не змінюється)
    - mem_calib.json: `hp_anchor_addr=268694140`, `hp_anchor_sub=15816`, `hp_off=8`, `max_hp_off=0`
  - **Full-process scan fallback** (HpAutoCalib): сканує всі r-w регіони /proc/pid/maps, шукає пари (cur,max)
  - ShadowMode avg diff 50% = артефакт logMobComparison; HP discrepancies = 0 (|mem_pct - ocr_pct| ~1.3%)
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
- **MR50** — `m_atk_unreachable_streak`: після 5 послідовних unreachable без kills →
  ігноруємо minimap_close_threat, форсуємо повний цикл (Pokemon macro + patrol)
- **MR51** — `m_atk_streak_force_count`: після 3 force-циклів (~75с) → ESC + `has_target=false`
  → patrol сам рухається та знаходить нову ціль (break 20хв Pokemon-loop)
- **MR52** — два критичних фікси:
  - `condNeedsRest`: не відпочивати < 15с після kill; hp_threshold 70%→45%
    - Root cause смерті: атакер > 70px minimap → close_threat=false; потіон стабілізує HP → hp_falling=false → бот застряє в Rest 51с і гине
    - RL-F "minimap=1.0" = кількість dots (feature[5]=dots.size()/5), НЕ minimap_close_threat!
  - KL-HP coords fallback: якщо `m_mem_player.valid=false` але `m_player_base` є → `refreshPlayerXYZ()` → `gs.coords_valid=true`
- **MR53** — MemReader UseKLBase=true; PosX/Y/Z_Offset=0x24/0x28/0x2C → coords_valid з пам'яті
- **MR54** — MemReader AutoCalibratePlayer() — scan playerBase+0x00..0x300 → mem_calib.json
- **MR55** — KL-HP: фільтр dist<100 (виключає self/player) + throttle логу при зміні цілі
- **MR58** — `actTgtF2AndMacro`: SKIP коли `has_target=1 && streak=0`; `[F2] SEND/SKIP` логування
- **MR60** — `m_atk_mem_hp_abs = absHp` завжди (fix false stuck_streak3 при активному DPS)
- **MR61** — KL-HP вибір моба по hp% match (±15% від OCR) з fallback nearest
  - Раніше: nearest-by-dist міг обрати іншого моба (B), B.HP незмінний → false unreachable
- **MR62** — QA frame capture → зовнішній демон `qa/frame_capture.py` (розділення відповідальності)
- **MR63/63b** — `qa/video_record.py`: ffmpeg x11grab запис сесій + нарізка кліпів
- **MR64/64b/64c/64d** — `launch_qa.sh`: єдиний старт бот + frame_capture + video_record
- **MR65** — `tgtHandlePatrolAndRotate`: force escape після >20с без руху (стіна + "Ціль не видно")
- **MR66** — XSendEvent hybrid input backend (Linux):
  - Keyboard → `XSendEvent(XKeyEvent)` до вікна гри (без глобального grab)
  - Mouse move → XTest (DirectInput ігнорує XSendEvent MotionNotify)
  - Mouse buttons → `XSendEvent(XButtonEvent)` з window-relative координатами
  - `platform.h` — централізовані платформо-залежні типи (pid_t, ssize_t)
  - ScrollLock зупинка: XQueryKeymap читає фізичний стан → не залежить від XSendEvent
- **MR67** — Windows порт Steps 2-11:
  - `ProcessMemory.h`: `ReadProcessMemory`; `MemReader.cpp`: Toolhelp32/EnumProcessModules
  - `offset_scanner`, `knownlist_reader`: VirtualQueryEx; `Notify.cpp`: CreateProcess curl.exe
  - `CMakeLists.txt`: cross-platform split (Linux/Windows)
- **MR68** — Hybrid mouse buttons → XTest (Wine ігнорує XSendEvent ButtonPress)
- **MR69** — `--watch-pos`: alive heartbeat щосекунди
- **MR70** — перекалібровка: OFF_OBJ_X/Y/Z `0x90→0x24`; region scan `0x3F0000-0x500000 → 0x300000-0x350000` (stride=0x5C0)
- **MR71** — cleanup непідтверджених offsets і мертвого коду (NAME/LEVEL/MP=0, readAllAsChars видалено)
- **MR72** — `safePct()` guardrail (mem HP поза [0,100] → OCR fallback); `[Shadow]` spam fix (== threshold)
- **MR73** — WalkForward вилучено з GeoNav; hp_u32 filter 500k→2k
- **MR74** — crash при shutdown fix: detach() → use-after-free; fix: stored thread + abortScan() + join
- **MR75** — dx stability tracking; KL-HP `absHp<10 && hpMax=0` ігноруємо; HP_Threshold 70→45%
- **MR76** — buff під час grace (`m_buff_after_death`); `m_close_unreachable_count` ліміт 3 + WalkForward(4000); crash fix SetLogCallback(nullptr)
- **MR77** — KL-HP фільтр dist>5000 → виключаємо garbage coords
- **MR78** — `--diff-scan` двофазне калібрування HP/MP/CP (snapshot1→урон→snapshot2→diff→mem_calib.json)
- **MR79** — `RecordDeath()` перенесено в actDead Фаза 0 (1 раз/смерть); fix хибного D-лічильника при HP=1%
- **MR80** — HP гравця ПІДТВЕРДЖЕНО в бойових умовах (div. вище)

## Оптимізаційний спринт (P1–P6, 2026-05)
- **P5** — DetectFarNPCs removed: `m_hsv_frames[5]` + `m_diffs[15]` (~5MB, 97 рядків) видалено; Eyes() → {}; Close() → {}
- **P1** — NPC кешування: `gs.npcs` заповнюється раз на тік у Brain::updateGameState(); BT не викликає DetectNPCs() напряму
- **P2** — WorldState `std::shared_mutex`: читачі (mobs/target/aliveCount/hasLootNearby) — shared_lock; записувачі — exclusive lock_guard
- **P3** — `WorldState::copyMobsTo(out)` — копіює m_mobs прямо в out під shared_lock (без тимчасового вектора)
- **P4** — nav_flow_detection = false за замовчуванням; call sites вже guarded → зміни не потрібні
- **P6** — UAF fix: `Notify::~Notify()` spin-wait до 10с на m_pending==0; `Input::~Input()` spin-wait до 2с на m_threads==0
- **CI** — `.github/workflows/ci.yml`: ubuntu-22.04, debug-fast build + run-san (Xvfb)
- **AllocStats** — `src/AllocStats.h/.cpp`: operator new/delete atomic counters; тільки в qa-debug (`-DRDGA1BOT_ALLOC_STATS=1`)
- **CMakePresets** — debug-fast (build_debug/, ~2хв), release (build/), qa-debug (build_qa/, ASan+UBSan, ~15хв)

## CATHODE-inspired стратегічний шар (v1.6)
- **Blackboard** (`src/Blackboard.h/.cpp`) — lock-free: 12 float + 6 int + 10 bool (bit-packed atomic<uint64_t>)
  - `syncFromGameState(gs)` — Brain оновлює кожен тік; Director-owned слоти (ZONE/MOOD/DIRECTIVE) не перезаписує
- **DirectorSystem** (`src/DirectorSystem.h/.cpp`) — аналіз 500ms: KPM rolling 60s, death streak, zone scoring
  - Директиви: FARM_HERE / REPOSITION / REST_FIRST / RETURN_TO_ZONE / FLEE
- **BotMood** (`src/BotMood.h`) — NEUTRAL/AGGRESSIVE/CAUTIOUS/FLEE/SUSPICIOUS + MoodManager hysteresis (×3, FLEE миттєво)
  - MoodRewardScale: AGGRESSIVE kill×1.25/death×0.80; CAUTIOUS kill×0.80/death×1.50; FLEE kill×0.50/death×2.00
- **SenseSystem** (`src/SenseSystem.h`) — tiered perception MINIMAL→STANDARD→HEIGHTENED→FULL (inline evaluator)

## Критичні правила (НІКОЛИ не порушувати)
- W/S/A/D — НЕ використовувати (відкривають чат L2), рух тільки стрілками
- UseForKillDetect=false НАЗАВЖДИ (баг: fake kills)
- Auto-loot server-side: LOOTING = ESC+300ms (без pickup key)
- XSendEvent hybrid backend для key/mouse injection (не xdotool --window)
- Mouse buttons — XTestFakeButtonEvent (не XSendEvent — Wine ігнорує ButtonPress, MR68)
- ScrollLock = зупинка бота (через XQueryKeymap — читає фізичний стан, не залежить від XSendEvent)
- Нові фічі: завжди feature flag в INI (Enabled=false за замовчуванням)
- Мінімальні цільові зміни — не робити broad rewrites
- BT tick() сигнатура: `std::string tick(GameState& gs)` — НЕ міняти
- m_children[] масив — НЕ переставляти під час виконання (ламає BTState)

## Ключові файли
- src/Brain.cpp               — координатор (сприйняття + потіони + dispatch)
- src/BotBehaviorTree.h/.cpp  — Farm BT + RL + Target піддерево
- src/BehaviorTree.h/.cpp     — Stackless VM (BTNode 24B, BTState 8B)
- src/game_state.h            — GameState struct (npcs cache + blackboard ptr)
- src/Blackboard.h/.cpp       — lock-free shared state (atomic float/int/bool)
- src/DirectorSystem.h/.cpp   — стратегічний AI, 500ms cadence, ZoneRecord
- src/BotMood.h               — BotMood enum + MoodManager + MoodRewardScale
- src/SenseSystem.h           — SenseSet evaluator (inline, no alloc)
- src/world_state.h/.cpp      — shared_mutex WorldState + copyMobsTo + bgLoop
- src/MemoryValidator.h/.cpp  — валідація PlayerState/L2Character/coords
- src/ShadowLogger.h/.cpp     — A/B Memory vs OCR → JSONL лог
- src/LinearQModel.h/.cpp     — Q(s,a)=W^T*phi(s), IRLS+Huber, 6 дій
- src/LearningWorker.h/.cpp   — async IRLS thread
- src/FeatureExtractor.h      — 10 ознак GameState → Eigen::VectorXf
- src/ExperienceBuffer.h      — циклічний буфер Experience
- src/RewardCalculator.h      — reward function + MoodRewardScale
- src/offsets_config.h        — ElmoreLab Kamael offsets (відкалібровано MR70)
- src/Intercept.h             — cross-platform input interface; LinuxInputBackend enum
- src/Intercept_Linux.cpp     — XSendEvent hybrid backend (MR66/68)
- src/platform.h              — платформо-залежні типи: pid_t, ssize_t
- src/ProcessMemory.h         — ReadProcessMemory (Win) / process_vm_readv (Linux)
- src/Notify.h/.cpp           — Telegram async Send + ~Notify() join (P6)
- src/Input.h/.cpp            — event queue + Send() async + ~Input() join (P6)
- src/AllocStats.h/.cpp       — operator new/delete counters (qa-debug only)
- third_party/eigen/          — Eigen 3.4.0 header-only
- CMakeLists.txt              — cross-platform CMake (Linux + Windows)
- CMakePresets.json           — debug-fast / release / qa-debug presets
- rdga1bot.example.ini        — всі опції з коментарями
- launch_qa.sh                — єдиний старт: бот + frame_capture + video_record
- qa/qa_monitor.py            — QA daemon (IsolationForest + session filtering)
- qa/frame_capture.py         — зовнішній демон скріншотів
- qa/video_record.py          — ffmpeg запис/нарізка сесій
- .github/workflows/ci.yml   — CI: ubuntu-22.04, debug-fast + run-san

## НЕ включати в білд
Runloop.cpp — legacy від l2cvbot (Options.cpp/.h видалено в MR36)

## MemPalace — контекст між сесіями
На початку кожної сесії:
  mp --palace ~/l2bot/rdga1bot/memory/palace wake-up
Перед кожним MR:
  ./scripts/mp-snapshot.sh "опис змін"

## Детальний контекст (читай за потребою)
- Архітектура + BT + CATHODE шар + CMakePresets: cat CLAUDE_ARCH.md
- Повна історія MR:                              cat CLAUDE_HISTORY.md
- Memory offsets + технічні факти:               cat CLAUDE_OFFSETS.md
