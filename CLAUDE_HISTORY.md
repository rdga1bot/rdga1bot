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
### ✅ QA Monitor (2026-04-12): Python daemon, IsolationForest аномалії, MemPalace bridge, replay 22 сесій / 57799 kills baseline
### ✅ MR23 (2026-04-12): Eigen 3.4.0, LearningConfig, FeatureExtractor (10 features), ExperienceBuffer, LinearQModel (IRLS+Huber), RewardCalculator
### ✅ MR24 (2026-04-12): LearningWorker — async IRLS thread (аналог GeodataWorker)
### ✅ MR25 (2026-04-12): RL інтеграція в BotBehaviorTree — initRL/shutdownRL, rlPreTick/rlPostTick, condNeedsBuff override, kill/death/buff/fail сигнали, Dashboard RL рядок
### ✅ MR26 (2026-04-12): MemoryValidator + blindScan(timeoutMs) + ShadowLogger (A/B Memory vs OCR JSONL, ShadowMode=false за замовч.)
### ✅ MR27 (2026-04-12): actTarget (456 рядків) → 6 приватних tgtHandle* instance methods (без зміни поведінки)
### ✅ MR28 (2026-04-12): Target замінено Selector піддеревом з 7 BT вузлів (actTgtInit..actTgtPatrol), ~22 вузли загалом
### ✅ MR29 (2026-04-12): RL активовано ([Learning] Enabled=true); condNeedsRest + tgtHandlePatrolAndRotate RL overrides (RestNow, Patrol boost)
### ✅ NavMesh tools (2026-04-12): scripts/navmesh_preview.py (2D scatter + кластер аналіз), scripts/navmesh_3d.py (Detour binary → Poly3DCollection 3D рендер)
### ✅ NavMesh cleanup (2026-04-12): navmesh_points.pts очищено від 343 артефактів (origin junk / NaN), navmesh_loa.pts — тільки LoA кластер (648 точок), navmesh_loa.bin побудовано (304 полігони)
### ✅ MR30 (2026-04-12): RL log routing → Brain::Log() через LogFn callback (LearningWorker mutex-guarded); weights.json збереження при shutdown (m_rl_weights_file замість hardcoded)
### ✅ MR31 (2026-04-12): condIsDead додано `!s_self->inGrace()` — запобігає 3 death episodes з однієї смерті через memory lag після respawn
### ✅ MR32 (2026-04-12): blindScan coordinate filter змінено з `|X|>30000 OR |Y|>30000` на `|X|<1000 AND |Y|<1000` — fix сумісності з ToI (X<30000) та LoA
### ✅ MR33 (2026-04-12): loadWeights() двопрохідна валідація — перший прохід читає num_features/num_actions, відхиляє несумісний файл зі старт-з-нуля
### ✅ MR34 (2026-04-12): RL повні overrides для всіх 6 дій (TargetNearest/Weighted/NavMemory/Patrol/RestNow/BuffNow) + softmax confidence ∈ (0,1] замість maxCoeff
### ✅ MR35 (2026-04-12): Аудит + playerBaseCache fix — main.cpp "Спроба 0" використовує offsets.json playerBaseCache з XYZ валідацією до blindScan
### ✅ MR36 (2026-04-12): Видалено Options.cpp/.h — 135 рядків dead code (legacy l2cvbot, не в build.sh, не імпортується)
### ✅ MR37 (2026-04-12): Feature debug log — FeatureLogInterval=300 в [Learning]; rlPreTick() логує [RL-F] з 10 ознаками + eps/conf/action через Brain::Log()
