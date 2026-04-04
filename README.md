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
- Optical flow (Lucas-Kanade) на мінімапі — flow-stuck detection
- Патруль PatrolPath (`F2000,R500,...`) при порожній мінімапі
- Memory-based навігація: координати XYZ + heading (опціонально, `[Navigation] Enabled=true`)
- Геодата L2J формат: A* pathfinding по `.geo` файлах (опціонально, `[Geodata] Enabled=true`)

**Детекція**
- OpenCV XShm screen capture (lazy HSV конвертація, ~30-60 FPS)
- HP/MP/CP бари персонажа і HP бар цілі (HSV, конфігуровані в INI)
- Auto-calibration TargetStatusWnd: автовизначення posX при першому таргеті
- F12 → `calibrate_*.png` + HSV auto-suggest в лозі

**Memory reading (KnownList)**
- `process_vm_readv` — без root, без Cheat Engine, без Windows API
- `blindScan()` — автономний пошук PlayerBase без відомих offsets
- Region scan heap: XYZ triplet scan → тип/HP/isDead/ім'я/level
- Fallback `readAllAsChars()` якщо `objTypeOff` не відкалібровано
- Thread-safe WorldState (snapshot copy під mutex, bgLoop scan кожну 1с)
- Зважений вибір цілі: `[WeightedTargeting]` — scoring по відстані/HP/freshness
- `--dump-objects` / `--calibrate [--name "X"]` — калібровка без Cheat Engine

**Мультипоточність**
- VisionWorker: async DetectNPCs + DetectMinimap у окремому потоці (Core 2, `[Threading] VisionThread`)
- GeodataWorker: async A* FindPath у окремому потоці (Core 3, `[Threading] GeodataThread`)
- CPU affinity для main thread + воркерів (опціонально, `CPUAffinity`)
- Всі воркери `Enabled=false` за замовчуванням — зворотна сумісність збережена

**Антидетект**
- RandomDelay: нормальний розподіл затримок між атаками, поворотами, ходьбою
- Конфігурується через `[Delays]` в INI (mean±std мс, `Enabled=false` за замовчуванням)

**Буфінг**
- ALT+B → template matching (`buff_tab.png`, `buff_profile.png`) → click профілю
- Fallback на координати якщо шаблон не збігся; retry через 120с
- Чекання виходу з combat state (~15с) перед відкриттям BBS

**Автоматика**
- Авто-зілля HP/MP/CP з кулдауном 5с (опціонально, гра auto-potion пріоритетніша)
- Dead detection: Enter → 20с → grace 30с → відновлення фарму
- Telegram сповіщення: смерть + статистика (через fork+curl)
- Авто-збереження stats кожні N kills

**TUI Dashboard (ncurses)**
- HP/MP/CP бари персонажа, HP цілі, kills/deaths, K/D ratio
- Права колонка: режим `[OPENCV]`/`[MEM]`/`[HYBRID]`, XY координати, кількість мобів
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
LogLevel = INFO            # DEBUG / INFO / WARNING / ERROR / NONE

[Character]
Class = Treasure Hunter    # Mage, Archer, Spoiler, Treasure Hunter, ...

[Targeting]
NextTargetKey = F2         # /nexttarget (основний)
MacroKeys = F7,F8,F9,F10,F11  # /target МобНейм резерв (F-keys!)
NearbyYThreshold = 0       # Screen-Y фільтр (0=вимк, 200=підземелля, 450=пустеля)

[Attack]
AttackKeys = F1
AttackWait = 0.5
AttackWatchdog = 20

[Buffs]
BuffEnabled = true
BuffUseAltB = true
BuffInterval = 600

[KnownList]
Enabled = true             # memory scan мобів без root
AutoScan = true            # blindScan() при старті

[Delays]
Enabled = false            # true = варіативні затримки (антидетект)
AttackMeanMs = 500.0
AttackStdMs  = 75.0

[WeightedTargeting]
Enabled = false            # true = зважений вибір цілі (потребує KnownList)

[Navigation]
Enabled = false            # memory-based навігація (потребує калібровки heading)

[Geodata]
Enabled = false            # A* по .geo файлах (потребує Navigation)
GeoPath = ./geodata/

[Threading]
Enabled       = false      # master switch
VisionThread  = false      # DetectNPCs async на Core VisionCore
VisionCore    = 2
GeodataThread = false      # FindPath async на Core GeodataCore
GeodataCore   = 3
CPUAffinity   = false      # прив'язати main thread до MainCore
MainCore      = 1
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
# Повний дамп об'єктів для визначення offsets
./rdga1bot --dump-objects

# Heading + name offset scan
./rdga1bot --calibrate
./rdga1bot --calibrate --name "Назва моба"
```

Докладно: [`CALIBRATION.md`](CALIBRATION.md)

## Архітектура

```
Brain.cpp/.h        — FSM: IDLE/TARGETING/ATTACKING/LOOTING/DEAD/BUFFING
Eyes.cpp/.h         — OpenCV детекція: HP/MP/CP бари, target HP, мінімапа, NPC
Hands.h             — дії: XTest keyboard/mouse, рух стрілками
Config.cpp/.h       — INI парсер + валідація + interactive TUI
Dashboard.cpp/.h    — ncurses TUI dashboard (3 колонки: бари / стат / mem info)
Capture_Linux.cpp   — XShm screen capture
Intercept_Linux.cpp — XTest backend (XTestFakeKeyEvent)
Window_Linux.cpp    — X11 window finding (кешований)
MemReader.cpp/.h    — читання HP/MP/CP/XYZ гравця з пам'яті Wine
OffsetScanner       — blindScan(), calibrateHeadingOffset(), findNameOffset()
KnownListReader     — region scan мобів, readName(), findMobByName()
WorldState          — thread-safe агрегатор KnownList для Brain
Geodata.cpp/.h      — L2J геодата: Load(), CanMoveTo(), IsWallBetween(), A* FindPath()
RandomDelay.h       — нормальний розподіл затримок (антидетект)
vision_worker.h/.cpp — async DetectNPCs+DetectMinimap (VisionWorker, окремий потік)
geodata_worker.h/.cpp — async A* FindPath (GeodataWorker, окремий потік)
Notify.cpp/.h       — Telegram через fork+curl
Stats.cpp/.h        — статистика сесії, JSON лог
```

## Вимоги

- Linux, X11 (не Wayland)
- Wine (протестовано: GE-Proton через Flatpak Lutris)
- g++ з підтримкою C++17
- OpenCV 4.x
- ncurses
- libx11, libxtst (XTest), libxext (XShm)
- curl (для Telegram, опціонально)
