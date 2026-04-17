# rdga1bot — v1.2

C++ бот для автоматизації фарму в Lineage II.  
Протестовано: ElmoreLab Kamael/Lionna, Arch Linux, Wine/Lutris (GE-Proton), X11.

## Результати (v1.2)

| Сесія | Kills | Deaths | Kill/хв |
|-------|-------|--------|---------|
| 31 хв (MR52) | 129 | 1 | 4.2 |
| 10 хв (MR51) | 37 | 0 | 3.5 |
| 125 хв (MR50) | 381 | 1 | 3.4 |
| Тиждень (~54 сесії, v1.1) | ~15 700 | — | 4–15 |

Ціль: **kill rate > 3/хв стабільно** — досягнуто.

---

## Можливості

### Бойовий цикл
- Автоматичний пошук і атака мобів через `/nexttarget` (F2) і `/target МобНейм` макроси
- Детекція смерті моба: OpenCV HP bar + KnownList memory read (instant, без debounce)
- Watchdog таймаут — перехід до лутингу якщо kill не детектується
- Pokemon sweep: `/target Pokemon` + `/useshortcut` після кожного kill
- Blacklist недосяжних мобів: HP-stable 5с → автоматичне блокування на 60с
- **MR50**: `m_atk_unreachable_streak` — після 5 unreachable без kills → форсуємо повний цикл (Pokemon + patrol), ігноруючи minimap_close_threat
- **MR51**: `m_atk_streak_force_count` — після 3 force-циклів (~75с) → ESC + `has_target=false` → patrol фізично переміщує бота (break 20хв Pokemon-loop)
- **MR52**: `condNeedsRest` не відпочиває < 15с після kill; `hp_threshold` 70%→45% (TH Vampiric Rage лікується атакуючи)

### Навігація
- Мінімапа Rotating Radar: детекція червоних/фіолетових точок мобів, ротація до цілі
- WalkForward по мінімапі (dy-based) + прогресивна ротація при застряганні
- Патруль PatrolPath (`F2000,R500,...`) при порожній мінімапі
- Memory-based навігація: координати XYZ + heading (`[Navigation] Enabled=true`)
- NavMesh Recast/Detour: `[NavMesh] Enabled=true` + зібрані точки руху

### Memory Reading (KnownList + MemReader)
- `process_vm_readv` — без root, без Cheat Engine, без Windows API
- `blindScan()` — автономний пошук PlayerBase; atomic abort при таймауті
- Region scan heap: XYZ triplet scan → тип/HP/isDead/ім'я/level (KL_MAX_OBJECTS=2000)
- HP читання (MR43): render_node+0x58 → game_obj → +0x14 = HP (uint32, NOT float)
- **MR53 UseKLBase**: MemReader отримує `playerBase` від KnownList без статичного `PlayerPtr`; `PosX/Y/Z=0x24/0x28/0x2C` → координати гравця з пам'яті без Cheat Engine
- **MR54 AutoCalibrate**: `AutoCalibratePlayer()` сканує `playerBase+0x00..0x300`, знаходить HP/MP/CP offsets через порівняння з OCR%; результат → `mem_calib.json` (автозавантаження при наступному старті)
- **ShadowMode**: логує порівняння OCR vs Memory (`[MemCalib]`, `[KL-HP]`) — діагностика без зупинки
- Thread-safe WorldState (snapshot copy під mutex, bgLoop scan кожну 1с)
- `--dump-objects` / `--calibrate [--name "X"]` / `--hp-calibrate` — калібровка без Cheat Engine

### BehaviorTree планувальник
- Stackless BT VM: `BTNode` (24 bytes), `BTState` (8 bytes), плоскі масиви — без heap, без рекурсії
- Гілки: Dead → Rest → Zone → Buff → Loot → Attack → Target Selector (~22 вузли)
- Target піддерево (MR28): Init → DeadTarget → Minimap → F2AndMacro → Navigation → GeoPath → Patrol
- `thread_local s_self` — static Action/Condition функції безпечно звертаються до стану
- GoogleTest suite: `tests/bt_test.cpp` + `tests/memory_test.cpp` (`make -C tests run`)

### Huber Q-Learning / RL (`[Learning] Enabled=true` за замовчуванням)
- Лінійна Q-функція: `Q(s,a) = W[:,a]^T * phi(s)`, 10 ознак × 6 дій
- IRLS з Huber-вагами — стійко до рідкісних смертей (викиди в reward)
- Async LearningWorker: IRLS в окремому потоці, не блокує main loop
- Epsilon-greedy exploration: `epsilon` від 1.0 до 0.05
- Ваги зберігаються в `weights.json`, автоматично завантажуються при старті

### QA Monitor
- Python daemon (`qa/qa_monitor.py`): аналіз логів, IsolationForest аномалії
- Режими: `--analyze-only stats.json --log session.log`, `--replay`, `--live`
- Фільтрація по сесії: розрізняє кілька сесій в одному stats-файлі (uptime/kills reset детектор)
- Аномалії: death_loop (лише "Фаза 0"), kl_hp_spike, targeting_failure_spike, slow_bt_tick

### Антидетект
- RandomDelay: нормальний розподіл затримок між атаками, поворотами, ходьбою
- Конфігурується через `[Delays]` в INI (mean±std мс, `Enabled=false` за замовчуванням)

---

## Швидкий старт

```bash
# Залежності (Arch Linux)
sudo pacman -S opencv gcc libx11 libxtst libxext curl ncurses

# Зібрати
bash build.sh

# Запуск
./launch.sh          # TUI налаштувань
./rdga1bot --quick   # без TUI (rdga1bot.ini)
```

## Конфігурація

Копіювати `rdga1bot.example.ini` → `rdga1bot.ini`.

```ini
[Learning]
Enabled = true        # Huber Q-Learning увімкнено за замовчуванням

[KnownList]
Enabled = true
AutoScan = true       # автоматичний blindScan PlayerBase

[MemReader]
Enabled = true
UseKLBase = true      # координати з playerBase (без Cheat Engine)
PosX_Offset = 0x24
PosY_Offset = 0x28
PosZ_Offset = 0x2C
ShadowMode = true     # логувати OCR vs Memory порівняння
# HP/MP/CP offsets знаходяться автоматично при першому запуску → mem_calib.json

[Potions]
HpThreshold = 45      # відпочинок тільки при < 45% HP (TH Vampiric Rage)
```

Повний список: [`rdga1bot.example.ini`](rdga1bot.example.ini)

## Калібровка

```bash
# Автоматична (при першому запуску з UseKLBase=true):
# → [MemCalib] знаходить HP/MP/CP offsets, зберігає в mem_calib.json

# Ручна діагностика:
./rdga1bot --calibrate              # дамп KnownList об'єктів
./rdga1bot --calibrate --name "Mob" # пошук по імені
./rdga1bot --hp-calibrate           # пошук HP offset мобів
```

## Клавіші

| Клавіша | Дія |
|---------|-----|
| ScrollLock | Зупинити бот (глобально) |
| P | Пауза / продовження |
| S | Налаштування (hot-reload) |
| R / Space | Скинути детекцію HP/MP/CP барів |
| F12 | Зберегти calibrate_*.png |

## Архітектура

```
Brain.cpp/.h           — диспетчер: сприйняття + потіони + BotBehaviorTree dispatch
BehaviorTree.h/.cpp    — stackless BT VM (BTNode 24B, BTState 8B)
BotBehaviorTree.h/.cpp — Farm BT + Target піддерево + RL хуки (MR27/28/50/51/52)
Eyes.cpp/.h            — OpenCV детекція: HP/MP/CP бари, target HP, мінімапа
Hands.h                — дії: XTest keyboard/mouse, рух стрілками
Config.cpp/.h          — INI парсер + валідація + interactive TUI
Dashboard.cpp/.h       — ncurses TUI
MemReader.cpp/.h       — HP/MP/CP/XYZ гравця; UseKLBase + AutoCalibratePlayer (MR53/54)
OffsetScanner          — blindScan, calibrateHeadingOffset, findNameOffset
KnownListReader        — region scan мобів (KL_MAX_OBJECTS=2000)
WorldState             — thread-safe агрегатор KnownList
LinearQModel.h/.cpp    — Q(s,a)=W^T*phi(s), IRLS+Huber, 6 дій
LearningWorker.h/.cpp  — async IRLS batch update thread
FeatureExtractor.h     — phi(s): 10 ознак з GameState → Eigen::VectorXf
ShadowLogger.h/.cpp    — A/B Memory vs OCR JSONL лог
qa/qa_monitor.py       — QA daemon: IsolationForest + session filtering
```

## Вимоги

- Linux, X11 (не Wayland)
- Wine (GE-Proton через Flatpak Lutris)
- g++ з C++17, OpenCV 4.x, ncurses, libx11, libxtst, libxext
- Python 3.x + scikit-learn (QA Monitor, опціонально)
- googletest (тести, опціонально)
