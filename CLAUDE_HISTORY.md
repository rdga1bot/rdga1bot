# rdga1bot — Історія MR

## СТАТУС ТЕСТУВАННЯ
### ✅ Варіант А (без гри) — ПРОЙДЕНО (2026-03-21)
### ✅ Варіант В (повний FSM цикл) — ПРОЙДЕНО (2026-03-22)
### ✅ Варіант Б (з грою) — ПРОЙДЕНО (2026-03-21)

## СТАТУС ГОТОВНОСТІ (2026-03-22)
### Підтверджено тестами (Flatpak Lutris + GE-Proton):
- XTest → Wine/L2 працює ✓
- XShm захоплення вікна працює ✓
- Window finding за WindowTitle = Lineage II працює ✓

## ВИКОНАНІ MR

### ✅ MR9+MR10: Тест фарму (2026-04-04) — 133 kills/10хв, 0 deaths
### ✅ MR11: Objectives Architecture (2026-04-04)
### ✅ MR12: Перенос Handle* → Objective підкласи (2026-04-05)
### ✅ MR13: Нові Objectives — Rest і Zone (2026-04-05)
### ✅ MR14: Архітектурні виправлення Objectives (2026-04-05)
### ✅ MR15: Видалення on_mob_unreachable callback (2026-04-05)
### ✅ MR16: HP reading fix ElmoreLab Kamael (2026-04-05)
### ✅ MR17: Оптимізація — Levenshtein, minimap throttle, JPS+ (2026-04-05)
### ✅ MR18: Breadcrumbs + NavMesh Recast/Detour (2026-04-06)
### ✅ MR19: MinimapNavEnabled=false, HpStableSkipBelow=5, KillLowHpTimeoutS=8
### ✅ MR20a: BehaviorTree VM — BTNode 24B, BTState 8B, без heap (2026-04-07)
### ✅ MR20b: BotBehaviorTree — Dead/Rest/Zone/Buff/Loot/Attack/Target (2026-04-07)
### ✅ MR20c: actTarget — повна міграція TargetObjective (2026-04-07)
### ✅ Фікси після MR20 (2026-04-11): dead detect hp<=1%, save navmesh on exit, offsets cache
### ✅ MR20c cleanup (2026-04-12): видалено ObjectiveManager + farm_objectives.h + objective.h (-1703 рядки)
### ✅ QA Monitor (2026-04-12): Python daemon, IsolationForest аномалії, MemPalace bridge
### ✅ MR23 (2026-04-12): Eigen 3.4.0, LearningConfig, FeatureExtractor (10 features), ExperienceBuffer, LinearQModel (IRLS+Huber), RewardCalculator
### ✅ MR24 (2026-04-12): LearningWorker — async IRLS thread
### ✅ MR25 (2026-04-12): RL інтеграція — initRL/shutdownRL, rlPreTick/rlPostTick, condNeedsBuff override
### ✅ MR26 (2026-04-12): MemoryValidator + blindScan(timeoutMs) + ShadowLogger (A/B Memory vs OCR JSONL)
### ✅ MR27 (2026-04-12): actTarget → 6 приватних tgtHandle* instance methods
### ✅ MR28 (2026-04-12): Target замінено Selector піддеревом з 7 BT вузлів (~22 загалом)
### ✅ MR29 (2026-04-12): RL активовано (Enabled=true); condNeedsRest + tgtHandlePatrol RL overrides
### ✅ MR30 (2026-04-12): RL logs → Brain::Log() через LogFn callback; weights.json при shutdown
### ✅ MR31 (2026-04-12): condIsDead + `!inGrace()` — запобігає 3 death episodes з однієї смерті
### ✅ MR32 (2026-04-12): blindScan coordinate filter `|X|<1000 AND |Y|<1000` (fix LoA/ToI)
### ✅ MR33 (2026-04-12): loadWeights() двопрохідна валідація num_features/num_actions
### ✅ MR34 (2026-04-12): RL повні overrides для всіх 6 дій + softmax confidence ∈ (0,1]
### ✅ MR35 (2026-04-12): playerBaseCache з offsets.json (Attempt 0 до blindScan)
### ✅ MR36 (2026-04-12): Видалено Options.cpp/.h — dead code legacy l2cvbot
### ✅ MR37 (2026-04-12): [RL-F] feature debug log кожні N тіків (FeatureLogInterval=300)
### ✅ MR38 (2026-04-12): blindScan reliability fix + scanmem-style maps parser
### ✅ MR39 (2026-04-12): scanmem-style region types + pread + priority scan
### ✅ MR41 (2026-04-12): CE reverse scan → forward scan; HP>0 filter + топ-30 ліміт
### ✅ MR42 (2026-04-12): діагностика: render_node дамп + game_obj pool scan + range ptr scan
### ✅ MR43 (2026-04-12): HP FIX: render_node+0x58 → game_obj → +0x14 = HP (uint32, NOT float!)
  - render_node+0x100 = interpolated X (не HP!), render_node+0x180 ≠ isDead
  - isAlive() тепер коректна; kill detection виправлена
### ✅ MR46b: зупинка атаки по dist (не minimap_dots); kClosePx: 35→70px
### ✅ MR47: m_buff_after_death — форс-баф після respawn ігнорує minimap_close_threat
### ✅ MR48: KL-HP nearest-mob (не min-HP); логування [KL-HP]; видалено false has_target override
### ✅ MR49: QA stats фільтрація по сесії (session_start з filename + uptime/kills reset detector)
### ✅ MR50: m_atk_unreachable_streak — 5 unreachable без kills → force повний цикл (Pokemon + patrol)
### ✅ MR51: m_atk_streak_force_count — 3 force-циклів → ESC + has_target=false → patrol переміщує бота
### ✅ MR52: два критичних фікси:
  - condNeedsRest: не відпочивати < 15с після kill; hp_threshold 70%→45%
  - KL-HP coords fallback: якщо m_mem_player.valid=false але playerBase є → refreshPlayerXYZ()
### ✅ MR53: MemReader UseKLBase=true; PosX/Y/Z_Offset=0x24/0x28/0x2C → coords_valid з пам'яті
### ✅ MR54: MemReader AutoCalibratePlayer() — scan playerBase+0x00..0x300 → mem_calib.json
### ✅ MR55: KL-HP фільтр dist<100 (виключає self) + throttle логу при зміні цілі
### ✅ MR58: actTgtF2AndMacro: SKIP коли has_target=1 && streak=0; [F2] SEND/SKIP логування
### ✅ MR60: m_atk_mem_hp_abs = absHp завжди (fix false stuck_streak3 при активному DPS)
### ✅ MR61: KL-HP вибір моба по hp% match (±15% від OCR) з fallback nearest
  - Root cause false unreachable: nearest-by-dist міг обрати іншого моба з незмінним HP
### ✅ MR62: QA frame capture → зовнішній демон qa/frame_capture.py (розділення відповідальності)
### ✅ MR63/63b: qa/video_record.py — ffmpeg x11grab запис + нарізка кліпів по log-подіям
### ✅ MR64/64b/64c/64d: launch_qa.sh — єдиний старт бот + frame_capture + video_record
### ✅ MR65: tgtHandlePatrolAndRotate: force escape після >20с без руху (fix стіна + "Ціль не видно")
### ✅ MR66: XSendEvent hybrid input backend:
  - Keyboard → XSendEvent(XKeyEvent); Mouse move → XTest; Mouse buttons → XSendEvent(XButtonEvent)
  - platform.h: pid_t/ssize_t; [Input] Backend = hybrid
### ✅ MR67: Windows порт Steps 2-11:
  - ProcessMemory.h: ReadProcessMemory; MemReader: Toolhelp32 FindPid + EnumProcessModules
  - offset_scanner + knownlist_reader: VirtualQueryEx; Notify: CreateProcess curl.exe
  - CMakeLists.txt: cross-platform split (Linux/Windows sources + libs)
### ✅ MR68: Hybrid mouse buttons → XTest (Wine ігнорує XSendEvent ButtonPress)
### ✅ MR69: --watch-pos alive heartbeat щосекунди (підтвердження offset без руху персонажа)
### ✅ MR70: перекалібровка L2Object offsets:
  - OFF_OBJ_X/Y/Z: 0x90→0x24 від playerBase (підтверджено --watch-pos)
  - Region scan: 0x3F0000-0x500000 → 0x300000-0x350000 (stride=0x5C0)
  - OFF_OBJ_TYPE=0x5C → завжди 0 (не фільтруємо)
### ✅ MR71: cleanup непідтверджених offsets (NAME/LEVEL/MP=0); видалено readAllAsChars, readItemsRegionScan, OFF_OBJ_TYPE фільтр
### ✅ MR72: Brain safePct() guardrail (mem HP поза [0,100] → OCR fallback); [Shadow] spam fix (== не >=); видалено false-positive mem_calib.json
### ✅ MR73: WalkForward вилучено з GeoNav (fix KL-HP false positives → крокував від моба); hp_u32 filter 500k→2k
### ✅ MR74: crash при shutdown fix — detach() blindScan → use-after-free; stored thread + abortScan() + join
### ✅ MR75: 5 виправлень після аналізу сесії (921 kills, 0 deaths, 3h):
  1. dx stability tracking — WalkForward при stuck dx ×3 (fix 315с gap)
  2. KL-HP false positive: absHp<10 && hpMax=0 → ігноруємо (139 false unreachable)
  3. ShadowLogger totalComparisons++ fix
  4. HP_Threshold 70→45%
  5. PERF slow ticks 300-656мс (inherent від ESC+200ms)
### ✅ MR76: buff під час grace (m_buff_after_death); m_close_unreachable_count ліміт 3 + force#3 WalkForward(4000); crash fix SetLogCallback(nullptr) перед dashboard.Shutdown()
### ✅ MR77: KL-HP фільтр dist>5000 → виключаємо garbage coords (false positive dist=112456)
### ✅ MR78: --diff-scan двофазне калібрування HP/MP/CP (snapshot1→урон→snapshot2→diff→mem_calib.json)
### ✅ MR79: RecordDeath() перенесено в actDead Фаза 0 (1 раз/смерть); fix хибного D-лічильника при HP=1%
### ✅ MR80: HP гравця ПІДТВЕРДЖЕНО — DSETUP.dll anchor 0x1003F27C; TUI "CONFIRMED hp=12874/15202"
  - Два механізми: hp_abs > hp_anchor_addr > hp_off
  - Root cause попереднього PENDING: зберегли 0x1003E7C замість 0x1003F27C (7 vs 8 цифр)

## ОПТИМІЗАЦІЙНИЙ СПРИНТ (P1–P6 + CI, 2026-05)

### ✅ P5: DetectFarNPCs removal
  - ~5MB звільнено: std::array<cv::Mat, 5> m_hsv_frames + std::array<cv::Mat, 15> m_diffs видалено
  - Eyes() → {} (пустий ctor); Close() → {} (не потрібен frame counter)

### ✅ P1: NPC кешування в GameState::npcs
  - Brain::updateGameState() заповнює gs.npcs раз на тік (async або sync)
  - BT вузли читають gs.npcs — НЕ викликають DetectNPCs() напряму
  - Умова: VisionWorker async result або (nearby_y_threshold > 0 && !target_macro_keys.empty())

### ✅ P2: WorldState shared_mutex
  - std::mutex → std::shared_mutex
  - Читачі: shared_lock (mobs, target, aliveCount, hasLootNearby …) — concurrent
  - Записувачі: lock_guard exclusive (bgLoop scan, update, setTarget …)

### ✅ P3: copyMobsTo — без тимчасового вектора
  - WorldState::copyMobsTo(out) — копіює m_mobs прямо в out під shared_lock
  - Brain: m_world->copyMobsTo(gs.kl_mobs) замість gs.kl_mobs = m_world->mobs()

### ✅ P4: nav_flow_detection — already guarded
  - Config.h: nav_flow_detection = false за замовчуванням
  - BotBT_Target.cpp:352-355: gs.cfg.nav_flow_detection ? eyes.GetMovementFlow() : -1.0f
  - Зміни не потрібні

### ✅ P6: UAF fix в Notify і Input
  - Notify::~Notify(): spin-wait до 10с на m_pending==0 (threads захоплюють this)
  - Input::~Input(): spin-wait до 2с на m_threads==0

### ✅ CI: .github/workflows/ci.yml
  - ubuntu-22.04: cmake --preset debug-fast → ninja → Xvfb + make -C tests run-san
  - Залежності: libopencv-dev, libx11-dev, libxtst-dev, libxext-dev, libncursesw5-dev, libgtest-dev, xvfb

### ✅ CI portability fixes (2026-05-13/14)
  - `std::fabsf` → `std::fabs` (9 файлів): GCC Ubuntu 22.04 не має `std::fabsf` в `<cmath>`
  - `std::sqrtf` → `std::sqrt` (3 файли: DirectorSystem.cpp, offset_scanner.cpp, tools/diag_dump.cpp)
  - Правило: ніколи не використовувати float-суфіксні варіанти (`fabsf`, `sqrtf`, `sinf` тощо) через `std::`

### ✅ Публічне репо + GPL-3 (2026-05-15)
  - rdga1bot/rdga1bot зроблено публічним на GitHub
  - Ліцензія GPL-3.0 (LICENSE файл присутній з initial commit)

### ✅ AllocStats (qa-debug preset)
  - src/AllocStats.h/.cpp: operator new/delete atomic counters під #ifdef RDGA1BOT_ALLOC_STATS
  - Brain heartbeat: [AllocStats] allocs=N total_kb=M кожні 600 тіків

### ✅ CATHODE-inspired стратегічний шар
  - Blackboard: lock-free atomic float/int/bool; sync з GameState кожен тік
  - DirectorSystem: mood+directive аналіз 500ms; ZoneRecord scoring; KPM rolling 60s
  - BotMood: NEUTRAL/AGGRESSIVE/CAUTIOUS/FLEE/SUSPICIOUS + hysteresis + MoodRewardScale
  - SenseSystem: tiered perception MINIMAL→FULL (inline evaluator)

### ✅ CMakePresets.json
  - debug-fast: -O0 -g (2хв); release: -O2 (5хв); qa-debug: ASan+UBSan+AllocStats (15хв)
