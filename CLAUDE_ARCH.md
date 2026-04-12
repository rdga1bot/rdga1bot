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
      ├── actTgtMinimap     — ротація до моба на мінімапі (зберігає m_tgt_map_ref)
      ├── actTgtF2AndMacro  — F2 nexttarget + macro fallback + pokemon
      ├── actTgtNavigation  — stuck detection + breadcrumbs + memory nav
      ├── actTgtGeoPath     — NavMesh + geodata waypoints + WalkForward
      └── actTgtPatrol      — patrol + rotate + explore (завжди Running)
```
`thread_local BotBehaviorTree* s_self` — static Action/Condition функції звертаються через pointer (single-threaded — безпечно).

Логіка Target Selector: кожен вузол повертає `Failure` (передати далі) або `Running` (чекати). `actTgtMinimap` обчислює `m_tgt_map_ref` — спільний стан для downstream вузлів.

## BT init() — КРИТИЧНО: BFS порядок
1. addChild(root, всі seq/action) підряд
2. addChild(seq, їх нащадки)

Порушення → circular childStart → infinite loop у VM.

## RL інтеграція (MR23-25, `[Learning] Enabled=false` за замовчуванням)

### Потік даних за тік:
```
rlPreTick(gs)          — extract phi(s), epsilon-greedy → m_rl_suggested_action
BT tick (незмінений)   — виконує гілки, встановлює RL сигнали (kill/death/buff/fail)
rlPostTick(gs)         — reward, push Experience, requestUpdate (async IRLS)
```

### RL компоненти:
- `LinearQModel`     — Q(s,a) = W[:,a]^T * phi(s), 10 features × 6 actions
- `ExperienceBuffer` — циклічний буфер N=1000, без alloc після init
- `LearningWorker`   — async IRLS thread (аналог GeodataWorker), Huber loss
- `FeatureExtractor` — 10 ознак: hp, mp, has_target, target_hp, kl_alive, minimap_dots, secs_kill, secs_buff, is_dead, in_grace
- `RewardCalculator` — +kill(1.0), -death(5.0), -targeting_fail(0.01), +buff(0.1), idle(-0.001)

### 6 тактичних дій RL:
```
TargetNearest=0, TargetWeighted=1, NavigateMemory=2,
Patrol=3, RestNow=4, BuffNow=5
```
RL **НЕ** переставляє гілки BT — тільки подає hints через `m_rl_suggested_action`.
Єдиний активний override: `condNeedsBuff` може форсувати BuffNow (confidence > 0.5).

### Збереження ваг:
- `weights.json` — простий JSON (num_features, num_actions, weights matrix)
- Авто-збереження кожні `SaveFrequency` kills та при shutdownRL()

## GameState
Тільки копії/значення + references + read-only callbacks.
НЕ зберігати між тіками.

## Ключові файли
```
src/Brain.cpp              — координатор: сприйняття + потіони + dispatch
src/BotBehaviorTree.h/.cpp — Farm BT + RL (initRL/shutdownRL/rlPreTick/rlPostTick)
src/BehaviorTree.h/.cpp    — Stackless VM (BTNode 24B, BTState 8B)
src/game_state.h           — GameState struct
src/LinearQModel.h/.cpp    — Q-функція, IRLS, save/load JSON
src/LearningWorker.h/.cpp  — async IRLS thread
src/FeatureExtractor.h     — phi(s): GameState → Eigen::VectorXf (10 dim)
src/ExperienceBuffer.h     — Experience{state,action,reward,next_state,done}
src/RewardCalculator.h     — reward function (константи явні)
src/MemoryValidator.h/.cpp — централізована валідація PlayerState/L2Character (MR26)
src/ShadowLogger.h/.cpp    — A/B Memory vs OCR → JSONL memory/shadow_logs/ (MR26)
src/world_state.h/.cpp     — KnownList читання пам'яті
src/knownlist_reader.h/.cpp — читання мобів з Wine процесу
src/Geodata.h/.cpp         — L2J .geo + A* + JPS+
src/navmesh_builder.h/.cpp — Detour runtime FindPath <1мс
src/vision_worker.h/.cpp   — async OpenCV (Core 2)
src/geodata_worker.h/.cpp  — async A* (Core 3)
third_party/eigen/         — Eigen 3.4.0 header-only
```

## Мультипоточність
```
Main thread (Core 1): Brain tick, BT, RL rlPreTick/rlPostTick (~30-60 FPS)
Core 2: VisionWorker  — async DetectNPCs + DetectMinimap
Core 3: GeodataWorker — async A* FindPath
Core 4: NavMeshWorker — async Detour FindPath
Core -: LearningWorker— async IRLS batch update (без affinity за замовч.)
```

## Config INI секції (feature flags)
```ini
[BehaviorTree]  Enabled=true     ← обов'язково
[Learning]      Enabled=false    ← Huber Q-Learning (MR23-25)
[MemReader]     ShadowMode=false ← A/B Memory vs OCR лог (MR26)
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
