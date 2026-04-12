# rdga1bot

C++ бот для автоматизації фарму в Lineage II.
Протестовано: ElmoreLab Kamael/Lionna, Arch Linux, Wine/Lutris (GE-Proton), X11.

## Можливості

**Бойовий цикл**
- Автоматичний пошук і атака мобів через `/nexttarget` (F2) і `/target МобНейм` макроси
- Детекція смерті моба: OpenCV HP bar + KnownList memory read (instant, без debounce)
- Watchdog таймаут — перехід до лутингу якщо kill не детектується
- Споіл/sweep для класу Spoiler (SpoilKey + SweepKey)
- Pokemon sweep: `/target Pokemon` + `/useshortcut` після кожного kill
- Blacklist недосяжних мобів: HP-stable 5с → автоматичне блокування на 60с

**Навігація**
- Мінімапа Rotating Radar: детекція червоних/фіолетових точок мобів, ротація до цілі
- WalkForward по мінімапі (dy-based) + прогресивна ротація при застряганні
- Патруль PatrolPath (`F2000,R500,...`) при порожній мінімапі
- Memory-based навігація: координати XYZ + heading (`[Navigation] Enabled=true`)
- Геодата L2J формат: A* pathfinding по `.geo` файлах (`[Geodata] Enabled=true`)
- NavMesh Recast/Detour: `[NavMesh] Enabled=true` + зібрані точки руху

**Детекція**
- OpenCV XShm screen capture (lazy HSV конвертація, ~30-60 FPS)
- HP/MP/CP бари персонажа і HP бар цілі (HSV, конфігуровані в INI)
- Auto-calibration TargetStatusWnd: автовизначення posX при першому таргеті
- F12 → `calibrate_*.png` + HSV auto-suggest в лозі

**Memory reading (KnownList)**
- `process_vm_readv` — без root, без Cheat Engine, без Windows API
- `blindScan()` — автономний пошук PlayerBase без відомих offsets
- Region scan heap: XYZ triplet scan → тип/HP/isDead/ім'я/level
- Thread-safe WorldState (snapshot copy під mutex, bgLoop scan кожну 1с)
- Зважений вибір цілі: `[WeightedTargeting]` — scoring по відстані/HP/freshness
- `--dump-objects` / `--calibrate [--name "X"]` — калібровка без Cheat Engine

**BehaviorTree планувальник (MR20, MR27-28)**
- Stackless BT VM: `BTNode` (24 bytes), `BTState` (8 bytes), плоскі масиви — без heap, без рекурсії
- Гілки: Dead → Rest → Zone → Buff → Loot → Attack → Target Selector (7 вузлів, ~22 загалом)
- Target піддерево (MR28): Init → DeadTarget → Minimap → F2AndMacro → Navigation → GeoPath → Patrol
- `thread_local s_self` — static Action/Condition функції безпечно звертаються до стану

**Huber Q-Learning / RL (MR23-25, `[Learning] Enabled=false` за замовчуванням)**
- Лінійна Q-функція: `Q(s,a) = W[:,a]^T * phi(s)`, 10 ознак × 6 дій
- IRLS з Huber-вагами — стійко до рідкісних смертей (викиди в reward)
- Async LearningWorker: IRLS в окремому потоці, не блокує main loop
- Epsilon-greedy exploration: `epsilon` від 1.0 до 0.05, decay після кожної смерті
- Ваги зберігаються в `weights.json`, автоматично завантажуються при старті
- При `Enabled=false` — **нуль** впливу на поведінку, нуль overhead

**Мультипоточність**
- VisionWorker: async DetectNPCs + DetectMinimap (Core 2, `[Threading] VisionThread`)
- GeodataWorker: async A* FindPath (Core 3, `[Threading] GeodataThread`)
- LearningWorker: async IRLS batch update (`[Learning] Enabled=true`)
- Всі воркери `Enabled=false` за замовчуванням — зворотна сумісність збережена

**Антидетект**
- RandomDelay: нормальний розподіл затримок між атаками, поворотами, ходьбою
- Конфігурується через `[Delays]` в INI (mean±std мс, `Enabled=false` за замовчуванням)

**Буфінг**
- ALT+B → template matching (`buff_tab.png`, `buff_profile.png`) → click профілю
- Fallback на координати якщо шаблон не збігся; retry через 120с

**QA Monitor**
- Python daemon (`qa/qa_monitor.py`): аналіз логів, IsolationForest аномалії
- Режими: `--analyze-only`, `--replay` (batch), `--live` (daemon під час фарму)
- MemPalace bridge: ChromaDB векторна БД для cross-сесійного контексту
- Baseline: 57799 kills, 22 сесії, 190 аномалій

**Автоматика**
- Авто-зілля HP/MP/CP з кулдауном 5с
- Dead detection: Enter → 20с → grace 30с → відновлення фарму
- Telegram сповіщення: смерть + статистика (через fork+curl)
- Авто-збереження stats кожні N kills + `/tmp/rdga1bot_stats.json` live export

**TUI Dashboard (ncurses)**
- HP/MP/CP бари персонажа, HP цілі, kills/deaths, K/D ratio
- Права колонка: режим `[OPENCV]`/`[MEM]`/`[HYBRID]`, XY координати, кількість мобів
- RL рядок: `[RL] eps=X loss=Y upd=Z` (тільки якщо `[Learning] Enabled=true`)
- Лог до 200 рядків з підсвіткою переходів стану
- Пауза (P), скидання барів (R/Space), налаштування (S), calibrate (F12)
- Глобальна зупинка: ScrollLock (XQueryKeymap, незалежно від фокусу)

## Результати тестів

| Тест | Kills | Deaths | Kill/хв |
|------|-------|--------|---------|
| 40 хв | 127 | 0 | 4.3 |
| 100 хв | 1511 | 0 | 15.1 |
| 3 год | 1940 | 0 | 11.0 |
| Тиждень (~54 сесії) | ~15 700 | — | 4–15 |

## Швидкий старт

```bash
# Залежності (Arch Linux)
sudo pacman -S opencv gcc libx11 libxtst libxext curl ncurses

# Зібрати
cd ~/l2bot/rdga1bot
bash build.sh

# Перший запуск (TUI налаштувань)
./launch.sh

# Звичайний запуск (з rdga1bot.ini)
./rdga1bot --quick

# Тривалий фарм з логом
./farm.sh
```

## Конфігурація

Копіювати `rdga1bot.example.ini` → `rdga1bot.ini`, відредагувати під себе.
`rdga1bot.ini` не комітується в git (містить Telegram токен).

```ini
[General]
WindowTitle = Lineage II
LogLevel = INFO

[Character]
Class = Treasure Hunter

[Targeting]
NextTargetKey = F2
MacroKeys = F7,F8,F9,F10,F11

[Attack]
AttackKeys = F1
AttackWait = 0.5
AttackWatchdog = 20

[Buffs]
BuffEnabled = true
BuffUseAltB = true
BuffInterval = 600

[KnownList]
Enabled = true
AutoScan = true

[BehaviorTree]
Enabled = true         # обов'язково для нового планувальника

[Learning]
Enabled = false        # true = Huber Q-Learning (потребує [BehaviorTree] Enabled=true)
LearningRate = 0.1
EpsilonStart = 1.0
WeightsFile = ./weights.json

[Delays]
Enabled = false        # true = антидетект варіативні затримки

[WeightedTargeting]
Enabled = false        # true = зважений вибір цілі (потребує KnownList)

[Threading]
Enabled = false
VisionThread = false
GeodataThread = false
```

Повний список параметрів: [`rdga1bot.example.ini`](rdga1bot.example.ini)

## Клавіші

| Клавіша | Дія |
|---------|-----|
| ScrollLock | Зупинити бот (глобально) |
| P | Пауза / продовження |
| S | Налаштування (hot-reload) |
| R / Space | Скинути детекцію HP/MP/CP барів |
| F12 | Зберегти calibrate_*.png + HSV auto-suggest |
| PrintScreen | shot.png |

## Калібровка KnownList (memory offsets)

```bash
./rdga1bot --dump-objects
./rdga1bot --calibrate
./rdga1bot --calibrate --name "Назва моба"
```

Докладно: [`CALIBRATION.md`](CALIBRATION.md)

## Архітектура

```
Brain.cpp/.h           — диспетчер: сприйняття + потіони + BotBehaviorTree dispatch
Eyes.cpp/.h            — OpenCV детекція: HP/MP/CP бари, target HP, мінімапа, NPC
Hands.h                — дії: XTest keyboard/mouse, рух стрілками
Config.cpp/.h          — INI парсер + валідація + interactive TUI
Dashboard.cpp/.h       — ncurses TUI (3 колонки: бари / стат / mem info + RL рядок)
BehaviorTree.h/.cpp    — stackless BT VM (BTNode 24B, BTState 8B, плоскі масиви)
BotBehaviorTree.h/.cpp — Farm BT + Target піддерево (MR27/28) + RL хуки
MemoryValidator.h/.cpp — валідація PlayerState/L2Character/coords (MR26)
ShadowLogger.h/.cpp    — A/B Memory vs OCR JSONL лог (MR26, ShadowMode=false)
LinearQModel.h/.cpp    — Q(s,a)=W^T*phi(s), IRLS+Huber, 6 дій, save/load JSON
LearningWorker.h/.cpp  — async IRLS batch update thread
FeatureExtractor.h     — phi(s): 10 ознак з GameState → Eigen::VectorXf
ExperienceBuffer.h     — циклічний буфер Experience{s,a,r,s',done}
RewardCalculator.h     — reward: kill/death/fail/buff/idle
third_party/eigen/     — Eigen 3.4.0 header-only (лінійна алгебра)
Capture_Linux.cpp      — XShm screen capture
Intercept_Linux.cpp    — XTest backend (XTestFakeKeyEvent)
Window_Linux.cpp       — X11 window finding (кешований)
MemReader.cpp/.h       — читання HP/MP/CP/XYZ гравця з пам'яті Wine
OffsetScanner          — blindScan(timeoutMs), calibrateHeadingOffset(), findNameOffset()
KnownListReader        — region scan мобів, readName(), findMobByName()
WorldState             — thread-safe агрегатор KnownList
Geodata.cpp/.h         — L2J геодата: Load(), CanMoveTo(), A* FindPath()
RandomDelay.h          — нормальний розподіл затримок (антидетект)
vision_worker.h/.cpp   — async DetectNPCs+DetectMinimap (Core 2)
geodata_worker.h/.cpp  — async A* FindPath (Core 3)
qa/qa_monitor.py       — QA daemon: IsolationForest + MemPalace bridge
Notify.cpp/.h          — Telegram через fork+curl
Stats.cpp/.h           — статистика сесії + JSON лог + /tmp live export
```

## Вимоги

- Linux, X11 (не Wayland)
- Wine (протестовано: GE-Proton через Flatpak Lutris)
- g++ з підтримкою C++17
- OpenCV 4.x
- ncurses
- libx11, libxtst (XTest), libxext (XShm)
- curl (для Telegram, опціонально)
- Python 3.x + scikit-learn (для QA Monitor, опціонально)
