# rdga1bot — Контекст для Claude Code

## Середовище
- OS: Arch Linux (CachyOS), X11, користувач rdga1
- Гра: Lineage 2, ElmoreLab (Kamael/Lionna), персонаж ManyaTheBond (Treasure Hunter/Dagger)
- Клієнт: Flatpak Lutris + GE-Proton/Wine
- Папка: ~/l2bot/rdga1bot/
- Build: bash build.sh && ./launch.sh

## Поточний стан (2026-04-12)
- **BotBehaviorTree** — єдиний планувальник (ObjectiveManager видалено в MR20c)
- **MR26** — MemoryValidator + blindScan(timeoutMs) + ShadowLogger (A/B JSONL, ShadowMode=false)
- **MR27** — actTarget → 6 приватних `tgtHandle*` instance methods
- **MR28** — Target Selector піддерево з 7 BT вузлів (~22 вузли загалом)
- **MR29** — RL активовано (`[Learning] Enabled=true`); condNeedsRest + tgtHandlePatrolAndRotate overrides
- **NavMesh tools** — `scripts/navmesh_preview.py` (2D), `scripts/navmesh_3d.py` (Detour binary → 3D mesh)
- **NavMesh LoA** — `navmesh_loa.pts` (648 чистих точок), `navmesh_loa.bin` (304 полігони, 985 трикутників)
  - Координатне відображення: L2_X→Recast_X, L2_Z(висота)→Recast_Y(up), L2_Y→Recast_Z
- **Наступні пріоритети**: live farm з RL активним → спостереження epsilon decay + weights.json

## Критичні правила (НІКОЛИ не порушувати)
- W/S/A/D — НЕ використовувати (відкривають чат L2), рух тільки стрілками
- UseForKillDetect=false НАЗАВЖДИ (баг: fake kills)
- Auto-loot server-side: LOOTING = ESC+300ms (без pickup key)
- XTest для key injection (не xdotool --window)
- ScrollLock = зупинка бота
- Нові фічі: завжди feature flag в INI (Enabled=false за замовчуванням)
- Мінімальні цільові зміни — не робити broad rewrites
- BT tick() сигнатура: `std::string tick(GameState& gs)` — НЕ міняти
- m_children[] масив — НЕ переставляти під час виконання (ламає BTState)

## Ключові файли
- src/Brain.cpp               — координатор (сприйняття + потіони + dispatch)
- src/BotBehaviorTree.h/.cpp  — Farm BT + RL + Target піддерево (MR27/28)
- src/BehaviorTree.h/.cpp     — Stackless VM (BTNode 24B, BTState 8B)
- src/game_state.h            — GameState struct
- src/MemoryValidator.h/.cpp  — валідація PlayerState/L2Character/coords (MR26)
- src/ShadowLogger.h/.cpp     — A/B Memory vs OCR → JSONL лог (MR26)
- src/LinearQModel.h/.cpp     — Q(s,a)=W^T*phi(s), IRLS+Huber, 6 дій
- src/LearningWorker.h/.cpp   — async IRLS thread (аналог GeodataWorker)
- src/FeatureExtractor.h      — 10 ознак GameState → Eigen::VectorXf
- src/ExperienceBuffer.h      — циклічний буфер Experience
- src/RewardCalculator.h      — reward function
- src/offsets_config.h        — ElmoreLab Kamael offsets (відкалібровано)
- third_party/eigen/          — Eigen 3.4.0 header-only
- build.sh                    — компіляція (g++, без cmake)
- rdga1bot.example.ini        — всі опції з коментарями
- qa/qa_monitor.py            — QA daemon (IsolationForest + MemPalace bridge)

## НЕ включати в білд
Runloop.cpp, Options.cpp — legacy від l2cvbot

## MemPalace — контекст між сесіями
На початку кожної сесії:
  mp --palace ~/l2bot/rdga1bot/memory/palace wake-up
Перед кожним MR:
  ./scripts/mp-snapshot.sh "опис змін"

## Детальний контекст (читай за потребою)
- Архітектура + BT дерево + Config INI: cat CLAUDE_ARCH.md
- Історія MR1-MR25:                    cat CLAUDE_HISTORY.md
- Memory offsets + технічні факти:     cat CLAUDE_OFFSETS.md
