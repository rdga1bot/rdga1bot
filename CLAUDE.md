# L2 Bot — Контекст для Claude Code

## Середовище
- OS: Arch Linux (CachyOS), X11, користувач `rdga1`
- Гра: Lineage 2, сервер ElmoreLab (Kamael/Lionna)
- Клієнт: **Flatpak Lutris** з **GE-Proton** (за замовчуванням, **підтверджено сумісним**)
  - XTest → Wine/L2 працює ✓
  - XShm захоплення вікна працює ✓
  - Window finding за `WindowTitle = Lineage II` працює ✓
- Папка проекту: `~/l2bot`
- Персонаж: ManyaTheBond, Human Fighter (Treasure Hunter / Dagger)

---

## АКТИВНИЙ ПРОЕКТ: rdga1bot (C++)

**Розташування:** `~/l2bot/rdga1bot/`
**Статус:** Основний бот. Python l2bot — legacy/довідка.

### Швидкий старт
```bash
cd ~/l2bot/rdga1bot
bash build.sh           # компіляція (g++, без cmake)
./rdga1bot              # запуск з ncurses TUI dashboard
./rdga1bot --quick      # запуск без TUI (читає rdga1bot.ini)
./rdga1bot --no-tui     # stdout debug + stdin CLI команди
./launch.sh             # запуск (без перевірки uinput — evdev видалено)
./launch.sh --setup     # примусовий TUI налаштувань
```

### Структура rdga1bot/
```
rdga1bot/
├── src/
│   ├── main.cpp            — головний цикл, ApplyConfig, signal handlers, CLI
│   ├── Brain.cpp/.h        — FSM + LogLevel + ReloadConfig + pause + perf timer
│   ├── Eyes.cpp/.h         — OpenCV детекція + SetColors + Robust + IsGroundAhead
│   ├── Hands.h             — дії (NextTarget/F2, AttackSkill, рух стрілками)
│   ├── Dashboard.cpp/.h    — ncurses TUI (бари, K/D, лог, налаштування overlay)
│   ├── Config.cpp/.h       — INI парсер + ColorConfig + Validate + всі секції
│   ├── Stats.cpp/.h        — статистика (kills/deaths/potions/uptime)
│   ├── Notify.cpp/.h       — Telegram через fork+curl (без libcurl)
│   ├── Input.cpp/.h        — евент-черга (XTest keyboard + mouse; evdev ВИДАЛЕНО)
│   ├── Intercept_Linux.cpp — XTest backend (evdev видалено 2026-03-22)
│   ├── Capture_Linux.cpp   — XShm screen capture
│   ├── Window_Linux.cpp    — X11 window finding
│   ├── MemReader.cpp/.h    — читання пам'яті L2 процесу (Wine) через process_vm_readv
│   └── FPS.h               — FPS counter
├── build.sh                — g++ пряма компіляція (cmake не потрібен)
├── launch.sh               — запуск з перевіркою /dev/uinput
├── rdga1bot.ini            — конфігурація (НЕ в git — містить Telegram токен)
├── rdga1bot.example.ini    — приклад з УСІМА опціями і коментарями (в git)
├── template/               — постійні шаблони для template matching
│   ├── buff_tab.png        — кнопка "Баффер" в ALT+B навігації
│   └── buff_profile.png    — профіль "tty1" в бафер-менеджері
├── tmp/                    — тимчасові файли: debug, calibrate, shot
│   ├── calibrate.png       — F12 повний кадр
│   ├── calibrate_hp/mp/cp.png — вирізані бари (F12)
│   ├── shot.png            — PrintScreen
│   └── buff_debug_*.png    — дебаг кадри бафу при невдачі шаблону
├── .gitignore
├── README.md
├── CHECKLIST.md
└── CMakeLists.txt
```

**НЕ включати в білд:** `Runloop.cpp`, `Options.cpp` — legacy від l2cvbot.

---

## Стейт-машина Brain.cpp

```
TARGETING → ATTACKING → LOOTING → TARGETING (цикл)
               ↓
             DEAD → (Enter → 20с → grace 30с) → IDLE → TARGETING
           BUFFING → IDLE (будь-якого стану)
```
*(IDLE → одразу TARGETING, практично не затримується)*

**TARGETING:**
- **Мертвий таргет (hp=0, звичайний моб)**: негайно ESC + Send(100мс) → наступна спроба
- **Мертвий таргет (hp=0, Pokemon)**: якщо `m_pokemon_targeted=true` — F3 вже виконав sweep через `/useshortcut 2 5` → чекаємо Delay(1500мс) анімацію → ESC. Лог: `[Spoil] Pokemon sweep (чекаємо анімацію)...`
  - Pokemon з'являється після вбивства будь-якого моба, вже з hp=0. `/nexttarget` (F2) мертвих мобів не вибирає — тільки F3 `/target Pokemon` може його вибрати.
- **Мінімапа**: кожен тік → `Eyes::DetectMinimap()` → якщо є dot з `selected=true` (фіолетовий ореол) — ротуємось до нього; інакше до найближчого червоного dot. `dx < -20` RotateLeft(120ms), `dx > +20` RotateRight(120ms), `|dx|≤20` → F2 одразу. Ліміт ротацій = 4 (захист від фейкових точок).
- **Pokemon macro**: `m_pokemon_macro_fired` flag — fires ОДИН РАЗ на attempt=1 (перша F2 спроба), скидається в EnterState(Targeting). `m_dead_cycles_total` — лічильник ESC-fallthrough циклів; після 3 → одразу macro замість F2.
- **Dead-target loop**: `m_dead_cycles_total` скидається при живому/відсутньому таргеті. Якщо `>= kDeadCyclesMacroSwitch=3` і є MacroKeys → `TargetMacro()` + return (обходимо F2).
- F2 `/nexttarget` — основний метод після ротації
- F7-F11 — резервні /target макроси (`MacroKeys`), **після 2 невдалих F2** (`kMacroFallbackAfter=2`), кожна 2-га спроба. Якщо попередній ATTACKING закінчився HP-stable (моб недосяжний) → `m_attack_was_unreachable=true` → макроси відкладаються до **15 спроб** (`kMacroFallbackAfterUnreach=15`) щоб навігація мала час вийти з кімнати. Скидається коли мінімапа покаже мобів.
- **Screen-Y фільтр** (якщо є MacroKeys): після таргету перевіряємо `DetectNPCs()` → якщо Selected моб видно але `cy < 300` (верхні ~40% екрана = далеко) → ESC і продовжуємо. Максимум 3 відхилення (`kMaxFarRejects`) — потім приймаємо будь-який таргет.
  - ⚠ Поріг 300 — для підземель/коридорів. Для пустелі (відкриті локації) підвищити до 450.
- **Fallback ротація** кожні 5 спроб (якщо мінімапа порожня): чергує RIGHT(350мс) і LEFT(350мс) — по черзі кожні 5 спроб
- **WalkForward** (dy-based): якщо моб по центру горизонтально (`|dx|≤20`) І спереду (`dy < -15`) → WalkForward(700мс) одразу. Fallback: кожні 4 спроби якщо мінімапа показує моба → WalkForward(400мс).
- **Видимість пошуку**: кожні 5 спроб виводиться `[TARGETING] Спроба N — ротація/шукаємо...` (INFO рівень)
- `Send(150мс)` між спробами (зменшено з 250мс)
- **Pokemon macro** (якщо `has_pokemon_key`): attempt==1 → натискає `pokemon_key` (F3) — макрос `/target Pokemon` + `/useshortcut 2 5`; встановлює прапор `m_pokemon_targeted = true`. Скидається в `EnterState(Targeting)`.

**ATTACKING:**
- Атака з `Config::GetAttackDelay(idx)` (per-skill або загальний attack_wait)
- Spoil на першій атаці (якщо Class=Spoiler)
- Re-detect target кожні 200мс (`m_eyes.ResetTarget()` + `DetectTarget()`)
- **Approach re-target**: після першої атаки (`!m_first_attack`) → кожні 1.0с надсилає ESC + F2, максимум 3 рази протягом перших 2с бою — щоб переключитись на ближчого моба поки персонаж біжить до поточного таргету. ESC потрібен бо L2 ігнорує `/nexttarget` при живій цілі.
  - **Умова (2026-03-23)**: `m_approach_entry_hp >= 90` — ретаргет тільки на СВІЖИХ мобах; `m_target->hp >= m_approach_entry_hp - 5` — не ретаргетимо якщо моб вже отримав удар.
  - **Перевірка нового таргету (2026-03-23)**: після ESC+F2, якщо новий моб вже має HP < 30% (майже мертвий, добивається іншим гравцем) → ESC + `m_approach_retarget_count--` → шукаємо знову. Умова: `SecsSince(m_approach_last_retarget) < 0.5` (перший тік після ретаргету).
  - Не робить `return` — атака продовжується на тому ж тіку.
- **Kill detection**: `HasTargetName()=false` → no_target_count++ → ≥8 тіків → LOOTING
- **Kill detection**: `hp<=2` → hp_zero_count++ → ≥3 тіки → LOOTING
- **Debounce**: обидва лічильники скидаються при появі валідного таргету
- **Мертвий таргет** (`m_first_attack=true` + hp=0 за 3 тіки) → TARGETING (без LOOTING, без kill)
- **HP-stable**: якщо HP моба не змінився 5с після першої атаки → моб недосяжний → встановлює `m_attack_was_unreachable=true` + `m_macro_idx++` → TARGETING (спробує інший макрос)
- Watchdog `attack_watchdog` секунд → LOOTING (поточне значення: 20с)

**LOOTING:**
- ESC + 300мс → TARGETING (гра auto-loot)
- `m_last_kill_time = Now()` — скидає post-combat cooldown для бафу
- Авто-збереження stats кожні `auto_save_kills` kills
- *(Pokemon sweep відбувається в TARGETING при виявленні hp=0 + m_pokemon_targeted)*
- `m_pokemon_targeted` скидається в `EnterState(Targeting)` → не переноситься на наступного моба

**DEAD:** Grace 10с при старті, 30с після respawn; HP=0 → 3 тіки debounce

**BUFFING:**
- Тригер: тільки в TARGETING + `SecsSince(last_buff) >= buff_interval` **+ `m_macro_attempts >= N`** де `N = post_combat_cooldown * 1000 / 150` (кількість consecutive targeting спроб ≈ секунди без мобів). Умови перевіряються в `Process()` до входу в BUFFING. На активних спотах kills кожні 2-5с → `SecsSince(last_kill)` ніколи не сягало cooldown → баф не відбувався. m_macro_attempts вимірює реальний "порожній" час.
- `buff_use_altb=true`: ALT+B → 1500мс → знайти "Баффер" (template або координати) → 800мс → знайти "tty" → 1с → ALT+B (закриває вікно)
  - **Template matching**: `buff_tab.png` + `buff_profile.png` в папці бота — якщо є, використовуються автоматично
  - **Fallback**: `BuffTabX/Y` + `BuffProfileX/Y` якщо шаблони не знайдено або не збіглися (threshold 0.75)
  - **Підготовка шаблонів**: відкрити ALT+B у грі → F12 → відкрити `calibrate.png` в GIMP → вирізати кнопку "Баффер" → зберегти `buff_tab.png`; те саме для "tty" → `buff_profile.png`; покласти в `~/l2bot/rdga1bot/`
- `buff_use_altb=false`: натискає `buff_keys` по порядку
- Перший баф: одразу при старті (`m_last_buff` ініціалізується годину тому)
- Перериваємо якщо HP < threshold або з'явився таргет

---

## Brain: API

```cpp
// Стан
Brain::State GetState() const;
static const char* StateName(State s);

// Пауза (P в TUI)
void TogglePause();
bool IsPaused(); // Process() нічого не робить; header показує ПАУЗА

// Hot-reload конфігурації (S в TUI → ShowSettings → ReloadConfig)
void ReloadConfig(const Config& new_cfg); // копіює Config за значенням

// Логування
enum class LogLevel { Debug=0, Info=1, Warning=2, Error=3, None=4 };
void SetLogLevel(LogLevel level);
void Log(const std::string& msg, LogLevel level = LogLevel::Info);
// Всі рядки мають префікс [HH:MM:SS]
// [DEAD] → Error; [STUCK]/Watchdog → Warning; [DEBUG] → Debug; [PERF] → Warning
// [WARNING] DetectMe() не знаходить HP бар → якщо DetectMe() fails 3с+

// Performance timer (автоматично в Process())
// [PERF] Повільний тік: Nмс — якщо тік > 50мс
```

---

## main.cpp: ключові патерни

### ApplyConfig() — єдина точка застосування Config
```cpp
ApplyConfig(cfg, hands, eyes, brain);
// Оновлює: всі клавіші Hands (включно m_move_*),
//          eyes.SetColors(), eyes.m_use_robust_bar,
//          brain.SetLogLevel()
// Викликати: при старті і після ShowSettings
```

### Signal handlers
```cpp
g_cleanup = [&]() { brain.GetStats().SaveToFile(); if (use_tui) dashboard.Shutdown(); };
std::signal(SIGINT, signal_handler);   // Ctrl+C → зберігає stats
std::signal(SIGTERM, signal_handler);  // kill → зберігає stats
```

### Exception recovery
Головний цикл обгорнутий у try/catch — авторестарт до 3 разів (5с пауза, eyes.Reset()).
`Intercept::InterceptionDriverNotFoundError` — завжди fatal.

### CLI команди (--no-tui режим)
stdin thread читає команди:
- `q` / `quit` — зупинити
- `p` / `pause` / `resume` — пауза/продовження
- `r` / `reset` — скинути HP/MP/CP bar detection
- `s` / `status` — вивести State/Kills/Deaths/Uptime

---

## Оптимізації продуктивності (2026-03-23)

### 1. Lazy HSV конвертація (Eyes.cpp)
- `Eyes::Open()` більше не викликає `cvtColor` — лише `m_bgr = bgr; m_hsv_full_ready = false`
- `EnsureFullHSV()` конвертує повний кадр (1366×768) лише при першому зверненні за тік
- `HSVForROI(roi)` — якщо full HSV вже є → sub-mat; інакше конвертує тільки roi (наприклад 179×5px)
- `HasTargetName()` і `DetectTargetHPDirect()` — `const` методи, використовують локальну конверсію `m_bgr(roi)` → `roi_hsv`
- `DetectMe()` — `HSVForROI()` для кешованих rect барів; `EnsureFullHSV()` перед `DetectMyBars()`
- **Результат**: тіки без DetectNPCs конвертують ~30K пікселів замість 1M

### 2. Blind spot перенесено в DetectNPCs()
- Раніше: `cv::circle` blind spot в `Eyes::Open()` — виконувався кожен тік
- Тепер: на початку `DetectNPCs()` — тільки коли справді треба (не кожен тік ATTACKING)
- Безпека: ROI DetectMe (0,0,180,80) і DetectTarget (598,0,179,46) не перетинаються з blind spot (683,384)±100px

### 3. XFlush батчинг (Intercept_Linux.cpp + Input.cpp)
- `SendKeyboardKeyEvent()`, `SendMouseMoveEvent()`, `SendMouseButtonEvent()` — `XFlush` прибрано
- `Intercept::FlushEvents()` — один `XFlush` наприкінці серії
- `Input::Send()` thread: `m_intercept.FlushEvents()` після циклу всіх подій

### 4. XGetInputFocus кешування (Intercept_Linux.cpp)
- `m_focus_verified` (mutable bool) — кеш фокусу для поточної серії подій
- `EnsureGameFocused()` перевіряє/встановлює фокус один раз, далі повертає одразу
- `ResetFocusCache()` викликається на початку `Input::Send()` thread
- **Результат**: N подій в одній серії → 1 XGetInputFocus замість N

### 5. Dashboard throttle (main.cpp)
- `dashboard.Update()` обмежено до 10 FPS (100мс інтервал)
- `last_dashboard_update` + `DASHBOARD_INTERVAL = 100ms`
- ncurses `wrefresh` (~1-2мс) не блокує головний цикл на кожному тіку

---

## Config: всі поля та секції INI

```ini
[General]
WindowTitle = Lineage II
Debug = false
LogLevel = INFO          # DEBUG/INFO/WARNING/ERROR/NONE

[Character]
Class = Treasure Hunter  # Mage, Archer, Spoiler, Treasure Hunter, ... (довільний рядок)

[Targeting]
NextTargetKey = F2       # /nexttarget макрос (основний)
MacroKeys = F7,F8,F9,F10,F11  # /target МобНейм резерв
NearbyYThreshold = 0     # Screen-Y фільтр (0=вимкнено, 200=підземелля, 450=пустеля)
MaxFarRejects = 5        # скільки разів відхилити "далекий" таргет → потім приймаємо

[Attack]
AttackKeys = F1
AttackWait = 0.5         # загальна затримка
AttackDelays =           # per-skill: 0.5,1.2,0.8 (порожньо = AttackWait)
AttackWatchdog = 20      # fallback (основний kill detection — HasTargetName, не HP%)
SpoilKey = F3         # не використовується (Class != Spoiler)
SweepKey = F4         # не використовується (Class != Spoiler)

[Loot]
LootEnabled = true       # false = пропустити 300мс очікування авто-лута
LootKey = F4             # не використовується (game auto-loot)
LootCount = 10           # не використовується

[Potions]
HP_Key =              # порожньо = вимкнено
MP_Key =              # порожньо = вимкнено
CP_Key =              # порожньо = вимкнено

[Buffs]
BuffEnabled = true       # false = ніколи не бафатись (ігнорує всі інші Buff* параметри)
BuffKeys =               # порожньо якщо BuffUseAltB=true
BuffInterval = 600       # 10 хвилин
BuffUseAltB = true       # ALT+B + mouse clicks
BuffPostCombatCooldown = 20  # чекати N сек після kill
BuffTabX = 773           # координати вкладки "Баффер" в ALT+B вікні (window-relative)
BuffTabY = 297
BuffProfileX = 841       # координати профілю "tty"
BuffProfileY = 252

[Special]
PokemonKey = F3          # /target Pokemon + /useshortcut 2 5 (порожньо = вимкнено)

[Stats]
AutoSaveInterval = 50    # авто-збереження кожні N kills (0 = вимкнено)

[Telegram]
BotToken = / ChatID =
NotifyOnDeath = true / StatsInterval = 3600

[Movement]               # ЗАВЖДИ стрілки — W/S/A/D відкривають чат в L2!
Forward = Up / Back = Down / RotateLeft = Left / RotateRight = Right

[Vision]
UseRobustBarDetection = true
WindowsInfoPath =        # шлях до WindowsInfo.ini (авто-визначення posX TargetStatusWnd)

[Colors_MyBars]          # HSV кольори барів персонажа
HPFromHSV = 0,75,55 / HPToHSV = 10,255,175
MPFromHSV = 88,30,35 / MPToHSV = 122,255,140
CPFromHSV = 12,80,50 / CPToHSV = 27,255,200

[Colors_Target]
HPFromHSV = 0,100,90 / HPToHSV = 12,220,160

[Colors_TargetCircles]
GrayBGR = 57,60,66 / BlueBGR = 107,48,0 / RedBGR = 0,4,132

[Colors_NPC]
NameFromHSV = 0,0,240 / NameToHSV = 0,0,255 / NameColorThreshold = 0.2

[Navigation]
StuckDetection = true    # frame diff після WalkForward → якщо не рухались → rotate
StuckThreshold = 2       # скільки тіків "не рухаємось" поспіль → escape rotation
WallDetection = false    # Sobel вертикальні ребра (experimental, false positives)
FlowDetection = true     # Lucas-Kanade flow на мінімапі (GetMinimapFlow): flow<0.3 2с → stuck

[Patrol]
PatrolEnabled = false
PatrolTriggerAttempts = 30   # спроб без мобів → старт patrol
# F=вперед B=назад L=ліво R=право + мілісекунди: F2000,R500,F1000
PatrolPath =

[MemReader]
# Читання пам'яті L2 процесу (Wine) через process_vm_readv (без root)
Enabled = false
ProcName = l2.exe
PlayerPtr = 0x0          # статична адреса в l2.exe (hex, відносно base)
PtrChain =               # 0x10,0x44 (порожньо = PlayerPtr вже адреса об'єкту)
HP_Offset    = 0x0       # offsets від початку PlayerObject (hex)
MaxHP_Offset = 0x0
MP_Offset    = 0x0
MaxMP_Offset = 0x0
CP_Offset    = 0x0
MaxCP_Offset = 0x0
PosX_Offset  = 0x0
PosY_Offset  = 0x0
PosZ_Offset  = 0x0
```

`Config::Validate()` — перевіряє MacroKeys, AttackKeys, конфлікти клавіш, HP threshold.
Повний приклад з коментарями: `rdga1bot.example.ini`

---

## Увімкнення/вимкнення модулів та функцій

| Параметр | Секція | Значення | Поведінка при вимкненні |
|----------|--------|----------|------------------------|
| `LootEnabled`       | `[Loot]`      | `true`/`false` | `false` → пропускає ESC+300мс після kill, одразу до TARGETING. Корисно для чистого XP фарму без збору лута. |
| `BuffEnabled`       | `[Buffs]`     | `true`/`false` | `false` → BUFFING стан ніколи не активується. Ігнорує BuffInterval, BuffKeys, BuffUseAltB. |
| `BuffKeys`          | `[Buffs]`     | порожньо | Якщо порожньо і `BuffUseAltB=false` → бафи вимкнені. |
| `BuffUseAltB`       | `[Buffs]`     | `true`/`false` | `false` → використовує `BuffKeys` замість ALT+B. |
| `HP_Key/MP_Key/CP_Key` | `[Potions]` | порожньо | Якщо порожньо → потіони для цього бару вимкнені. |
| `PokemonKey`        | `[Special]`   | порожньо | Якщо порожньо → Pokemon sweep вимкнено. |
| `MacroKeys`         | `[Targeting]` | порожньо | Якщо порожньо → Screen-Y фільтр теж вимкнений (він залежить від наявності MacroKeys). |
| `NearbyYThreshold`  | `[Targeting]` | `0` | `0` → Screen-Y фільтр повністю вимкнений (приймаємо будь-який таргет). |
| `UseRobustBarDetection` | `[Vision]` | `true`/`false` | `false` → середнє значення замість медіани (швидше, але менш стабільно). |
| `WindowsInfoPath`   | `[Vision]`    | порожньо | Якщо порожньо → posX TargetStatusWnd = 598 або авто-калібрування. |
| `AttackWatchdog`    | `[Attack]`    | секунди | Watchdog kill detection; збільшити для важких мобів. |
| `AutoSaveInterval`  | `[Stats]`     | `0` | `0` → авто-збереження вимкнено. |
| `StatsInterval`     | `[Telegram]`  | `0` | `0` → Telegram stats вимкнено. |
| `Debug`             | `[General]`   | `false` | `false` → не зберігає debug PNG кадри. |
| `LogLevel`          | `[General]`   | `NONE` | `NONE` → мовчазний режим. |
| `StuckDetection`    | `[Navigation]`| `true`/`false` | `false` → вимкнути frame-diff stuck detection. |
| `FlowDetection`     | `[Navigation]`| `true`/`false` | `true` → GetMinimapFlow() як третій сигнал руху після WalkForward (разом з IsCharacterMoving + GetMovementFlow). Надійніше у підземеллях з одноманітними текстурами. |
| `WallDetection`     | `[Navigation]`| `false` | `true` → Sobel wall detection (experimental). |
| `PatrolEnabled`     | `[Patrol]`    | `false` | `true` → патруль по PatrolPath коли мінімапа порожня N спроб. |
| `MemReader.Enabled` | `[MemReader]` | `false` | `true` → читати HP/MP/CP/XYZ з пам'яті Wine процесу замість OpenCV. Потребує правильних offsets. |

### Приклади конфігурацій

**Чистий XP фарм (максимальна швидкість):**
```ini
LootEnabled = false
BuffEnabled = false
NearbyYThreshold = 0
AttackWatchdog = 10
```

**Класичний фарм з бафами ALT+B:**
```ini
LootEnabled = true
BuffEnabled = true
BuffUseAltB = true
BuffInterval = 600
```

**Підземелля з навігацією:**
```ini
NearbyYThreshold = 0
FlowDetection = true
StuckDetection = true
StuckThreshold = 2
PatrolEnabled = false    # увімкнути якщо є прямі коридори
```

**Відлагодження детекції (підземелля):**
```ini
LogLevel = DEBUG
NearbyYThreshold = 0
MaxFarRejects = 3
FlowDetection = true
```

---

## ncurses TUI Dashboard

| Клавіша | Дія |
|---------|-----|
| Q | Зупинити бот (через ncurses, потрібен фокус TUI) |
| ScrollLock | Зупинити бот (глобально, через XQueryKeymap — працює завжди) |
| P | Пауза / продовження |
| S | Налаштування overlay (hot-reload) |
| R | Скинути HP/MP/CP bar detection |
| ESC (фізична) | Зупинити бот (IsReady guard) |
| PrintScreen | shot.png |
| Space | Скинути HP/MP/CP bar detection |
| F12 | calibrate_*.png + **HSV auto-suggest** в лозі |

Панелі:
- **Header**: стан + FPS + червоний блимаючий `ПАУЗА`
- **Status**: HP/MP/CP бари (████░░) + Target HP + `Kills/Deaths` + `K/D: X.X  Pots: N`
- **Log**: до 200 рядків, переходи стану [STATE] — жовтим
- **Footer**: підказки клавіш

---

## КРИТИЧНІ ТЕХНІЧНІ ФАКТИ

### Клавіші — ЩО ПРАЦЮЄ і ЩО НІ
- ✓ **XTest (XTestFakeKeyEvent)** — інжекція до сфокусованого вікна; бот сам фокусує L2 перед кожним натисканням
- ✓ **Стрілки ↑↓←→** — рух і поворот (**ПІДТВЕРДЖЕНО**)
- ✓ **Chase Camera**: LEFT/RIGHT = поворот персонажа І камери одночасно
- ✗ **evdev UInput** — ВИДАЛЕНО (було глобальним, заважало іншим вікнам)
- ✗ **W/S/A/D** — відкривають ЧАТ в L2, **НЕ використовувати для руху!**
- ✗ **Цифрові клавіші (0-9) і нумпад** — можуть друкувати символи в чат L2, **НЕ використовувати для макросів!** Використовувати F-keys.
- ✗ `xdotool key --window WINE_ID` — НЕ працює з Wine
- ✗ будь-яке використання миші для руху — гра блокує курсор

### XTest фокус-менеджмент (КРИТИЧНО)
- Перед кожним `SendKeyboardKeyEvent`: `XGetInputFocus` → якщо не L2 → `XSetInputFocus(L2)` без відновлення
- L2 залишається у фокусі постійно; TUI клавіші (Q/P тощо) через `XQueryKeymap` (глобально)
- **ScrollLock** = глобальна зупинка бота (читається через XQueryKeymap незалежно від фокусу)

### Рух персонажа (ПІДТВЕРДЖЕНО)
- `Hands::WalkForward/WalkBack/RotateLeft/RotateRight` — використовують `HoldKeyboardKey` (KeyDown→Delay→KeyUp), НЕ PressKeyboardKey (той робить швидкі тепи)
- Дефолт `m_move_*` — **завжди стрілки**, навіть після зміни Config
- LEFT/RIGHT = поворот персонажа і камери (Chase Camera mode)

### Авторотація камери — НЕМОЖЛИВА
- `evdev BTN_RIGHT + REL_X`, `xdotool mousedown 3` — Wine ігнорує (DirectInput)
- **Рішення**: стрілки LEFT/RIGHT (через HoldKeyboardKey)

### Архітектура головного циклу (main.cpp)
- `Input::Send(N)` — **неблокуючий**: запускає фоновий thread, main loop продовжується
- `IsReady()` → `m_threads == 0` — true коли фоновий thread завершився
- Головний цикл без `IsReady()` крутився б на 200-500+ FPS — марна робота
- **Fix (2026-03-22)**: `if (!hands.IsReady()) { sleep(5ms); continue; }` — пропускаємо захоплення кадру і OpenCV поки дії в польоті
- **Fix**: `Window::Find()` кешується (handle+rect) — пошук тільки при втраті вікна (`XQueryTree` надто дорогий щоб викликати 200 разів/сек)
- **Fix**: `bgr.clone()` → `m_bgr = bgr` (shallow ref-count) + blind_spot на HSV замість BGR
- **Fix**: `m_hsv.clone()` в `Eyes::Close()` прибрано — `DetectFarNPCs()` не використовується

### КРИТИЧНО: ncurses + OpenCV — порядок включення
ncurses.h визначає макроси (`PANORAMA`, `clear`, `refresh`, `move`, `timeout`, тощо)
що конфліктують з OpenCV заголовками.

**Правило для Dashboard.cpp:**
```cpp
#include "Dashboard.h"   // 1. СПОЧАТКУ — тягне OpenCV через Eyes.h
#include <ncurses.h>     // 2. ПОТІМ — ncurses (макроси тільки для тіл функцій)
```
**Правило для Dashboard.h:** НЕ включати `<ncurses.h>`. Тільки:
```cpp
struct _win_st;
typedef struct _win_st WINDOW;
```

### КРИТИЧНО: xdotool windowfocus скидає клавіші в Wine
C++ rdga1bot: evdev Send() не потребує windowfocus — проблеми немає.

### XShm screen capture
`XShmGetImage` завжди від (0,0): `bitmap.data = m_data + rect.y * stride + rect.x * bpp`

### HP/MP/CP детекція — Eyes.cpp
- Кольори конфігуровані через `[Colors_*]` в INI — без перекомпіляції
- `Eyes::SetColors(cfg.colors)` застосовує нові кольори + скидає кеш барів
- `CalcBarPercentValueRobust`: медіана по рядках 25%/50%/75% — стійко до артефактів
- `Eyes::IsGroundAhead()`: яскравість 60×20px зони внизу екрану; <30 → обрив/стіна
- F12 → `HSVSuggest()` → mean±2σ → рекомендовані HSV в лозі

### КРИТИЧНО: Mouse clicks — MoveMouseTo не MoveMouse
- `Hands::MoveMouseTo(point)` → `WindowPoint(point)` → додає `m_window_rect.x/y` → **правильні screen coords**
- `Input::MoveMouse(point)` → raw screen coords → **НЕ використовувати в Brain.cpp**!
- Вікно L2 постійно переміщується (користувач рухає) → hardcoded screen coords ніколи не надійні
- `hands.SetWindowRect()` оновлюється кожен тік в main.cpp (рядок 256)
- ⚠ Всі кліки миші в Brain.cpp — тільки через `m_hands.MoveMouseTo()`

### КРИТИЧНО: ALT+B баф — архітектура (виправлено 2026-03-22, 2026-03-24)
- `resolveClick()` → шукає шаблон (threshold=0.60, логує score%), fallback = window-relative coords
- Stage 0: ALT+B + Delay(1500)
- Stage 1: знайти "Баффер" (retry до 3x якщо не знайдено) + MoveMouseTo + click + Delay(2500)
  - `m_buff_tab_click_pos` зберігає де клікнули для delta-корекції profile coords
  - Якщо шаблон не знайдено → `m_buff_tab_fallback = true` → retry через 120с (не 600с)
- Stage 2: знайти "tty" (delta-корекція fallback відносно tab позиції) + MoveMouseTo + click + Delay(1000)
- Stage 3: ALT+B закрити
- Stage 4: якщо `m_buff_tab_fallback` → `m_last_buff = now - (interval-120)`, інакше `now`
- Шаблони: `template/buff_tab.png`, `template/buff_profile.png`
- Debug кадри при невдачі → `tmp/buff_debug_*.png`
- ⚠ **Fallback координати** (якщо шаблони не збіглись): `BuffTabX=773, BuffTabY=297, BuffProfileX=841, BuffProfileY=252`
  - Ці координати window-relative (MoveMouseTo додає offset вікна)
  - Були неправильні (2465/2532 з multi-monitor сесії) → баф не застосовувався + 600с чекання

### КРИТИЧНО: Target HP детекція
- **Реальний HP бар моба** в ElmoreLab Kamael має V=47-78 — темно-червоний. Fill S≈210 >> фон S≈118.
- **`DetectTargetHPDirect()`**: ROI `[m_target_wnd_x .. m_target_wnd_x+w, y+27..y+32]`; `inRange(S≥190, V≥30)` рахує fill-пікселі; знаменник `BAR_WIDTH_100=152`. Повертає 0..100 або -1.
  - ⚠ V поріг знижено **80→30** — темний бар (V=47-78) не фільтрується
  - ⚠ **Координати динамічні** — читаються з `WindowsInfo.ini` через `Config::windows_info_path`
  - ⚠ **Auto-calibration**: при fill=0 сканує весь рядок y=27..32 (~1366×5px). Guard: `scan_best>=8` + `|new_x - base|<=150px`. Без guard — стрибки до buff icons/UI (559→415→700→349 кожні 2хв). `m_target_wnd_x_base` = незмінна базова позиція з `SetTargetWnd()`.
- **`HasTargetName()`**: `[m_target_wnd_x .. m_target_wnd_x+w-2, y+2..y+22]`; S≤50 + V≥130 ≥5px → ім'я є.
- **`DetectTarget()` логіка**: HasTargetName()=false → {} ; DetectTargetHPDirect()≥0 → hp_direct ; fallback scan → hp=50.
- **Debounce в Brain**: hp≤5 потрібно 3 тіки; no_target потрібно 8 тіків (×100мс = 800мс).
- **Root cause знайдено (2026-03-23)**: TargetStatusWnd posX=539 (реальна), але код мав хардкод x=598. При HP≤25% fill bar (≈38px) закінчувався на x=577 — лівіше старого ROI → 0 пікселів → hp=0 → dead target → ESC → пропуск моба. Фікс: динамічний `m_target_wnd_x`.
- **Auto-calibration (2026-03-23)**: `DetectTargetHPDirect()` тепер при fill=0 сканує весь рядок y=27..32 по ширині екрана (~1366×5px). Якщо бар знайдено в іншому місці → `m_target_wnd_x` оновлюється автоматично (mutable). Лог: `[Eyes] TargetWnd авто-калібрування: x=OLD → NEW`. Розрізняє "моб мертвий" (fill=0 скрізь) від "координати неправильні" (fill=0 у відомій позиції але >0 деінде).

### КРИТИЧНО: m_my_bar_min_height = 7 (не 10!)
HP бар ElmoreLab має висоту лише ~10px. Ерозія з kernel=(1,10) знищує бар повністю.
Зменшено до 7 в `Eyes.h`. **Не підвищувати назад до 10** — bars не будуть знайдені!

### КРИТИЧНО: DetectMyBars() — архітектура детекції барів (2026-03-22)
1. **HP bar**: шукаємо в ROI `m_my_bar_search_w=180, m_my_bar_search_h=80` (точно StatusWnd=179×80).
   - ⚠ **НЕ збільшувати search_h вище 80!** Пустельний фон y=80-100 (H≈2, S≈119, V≈65) збігається з HP кольором і руйнує детекцію.
   - `m_my_bar_min_width = 30` — дозволяє детекцію при будь-якому % HP
   - HP rect розширюється до правого краю search area для коректного CalcBarPercentValue
   - `m_my_hp_color_to_hsv H=10` (не 20!) — щоб CP бар (H=12+) не зливався з HP
2. **CP і MP**: шукаємо **окремо** в Y-зонах відносно HP бару:
   - CP zone: `hp_y-15 .. hp_y-1` (вище HP бару)
   - MP zone: `hp_bottom+1 .. hp_bottom+20` (нижче HP бару)
   - Поріг: ≥10% від площі зони повинні збігатись з кольором
   - Це уникає злиття CP+MP через фрейм StatusWnd або фон гри
3. **FindMyBarContours**: erode+dilate kernel `(3, m_my_bar_min_height)` — ширина 3 (не 1!).
   - ⚠ **НЕ змінювати на (1,7)!** З kernel (1,7) вузькі 2px артефакти між CP і HP барами виживають ерозію → CP і HP зливаються в один контур h=22 > max_height=20 → бари не знайдені.
   - З kernel (3,7) вузькі стовпці (<3px) видаляються, CP і HP детектуються окремо.
⚠ Якщо UI перемістився (StatusWnd relocated) — змінити `m_my_bar_search_w/h` в Eyes.h.

### HSV калібрування ElmoreLab (ManyaTheBond, виміряно 2026-03-22, пустеля)
| Бар | H | S | V | Примітка |
|-----|---|---|---|---------|
| CP (жовтий) | 12-27 | 80-255 | 50-200 | |
| HP (червоний) | 0-10 | 75-255 | 55-175 | H max знижено 20→10 (щоб не зливатись з CP H=12+) |
| MP (синій) | 88-122 | 30-255 | 35-140 | S мін знижено з 80→30 |
| Target HP (реальний) | 0-12 | 100-220 | **47-78** | ⚠ темний, поза конфігом! |
| Фон пустелі | 0-20 | 75-255 | 55-175 | збігається з HP! — тому обмеження ROI критичне |

### Мінімапа ElmoreLab (виміряно 2026-03-22, calibrate.png 1366×768)
- Режим: **Rotating Radar** — гравець завжди ВГОРУ, карта обертається
- Розташування: верхній правий кут вікна
- ROI в window-coords: `x = w-185, y=0, w=185, h=165`
- Центр кола в ROI: `(95, 89)`, радіус R=78px
- Гравець: центр ± 12px (exclusion zone)
- **Моби (червоні dots)**: HSV `[0,100,80]–[20,255,255]` + `[165,100,80]–[180,255,255]` (S≥100; H upper 10→**20** — виміряно що H=10-19 дає 1417px точок vs H=0-9 лише 35px у підземеллях)
- **Вибраний моб (фіолетовий ореол)**: HSV `[130,80,80]–[165,255,255]` — з'являється після `/target МобНейм`. `MinimapDot::selected=true` якщо центр purple contour ≤15px від червоної точки. Пульсує (1-2 кадри on/off) — деградує до nearest red dot якщо off.
- Фільтр: `area >= 2.0` (знижено з 4.0 — малі точки у підземеллях), `dist > 12` (не гравець), `dist < R+5`
- Інтерпретація: `dx<0` → ліво, `dx>0` → право, `dy<0` → спереду, `dy>0` → позаду
- Поріг ротації: `|dx| > 20px` → RotateLeft/Right(120ms) (підвищено з 12 для уникнення false positive)
- Ліміт ротацій: 4 поспіль → зупиняємо (якщо після 4 ротацій моб не з'явився = фейк)
- Лог ротації (Debug): `[MAP] Вибраний моб ліворуч/праворуч` vs `[MAP] Найближчий моб`
- Screen-Y поріг NPC фільтру: конфігурується в .ini `NearbyYThreshold` (дефолт 200px); `MaxFarRejects` (дефолт 5). `0` = вимкнено.

### WindowsInfo.ini (координати UI ElmoreLab)
Файл: `/home/rdga1/deiceland/system/WindowsInfo.ini` (шлях задається в `[Vision] WindowsInfoPath`)
```
StatusWnd:       posX=0,   posY=0,   width=179, height=80
TargetStatusWnd: posX=539, posY=0,   width=179, height=46
```
⚠ **posX=539, НЕ 598** — якщо хардкодити 598, то при HP≤25% fill bar не потрапляє в ROI!
Config::Load() парсить цей файл і встановлює `target_wnd_x/y/w/h`; `ApplyConfig()` викликає `eyes.SetTargetWnd()`.

---

## LEGACY: Python l2bot (довідка)

**Розташування:** `~/l2bot/` — НЕ активний.

### HP/MP/CP детекція — Python (BGR, для довідки калібрування)
```python
HP_COLOR_LOW  = [30,  40,  90]   HP_COLOR_HIGH = [80,  90,  160]
MP_COLOR_LOW  = [100, 70,  50]   MP_COLOR_HIGH = [180, 130, 110]
CP_COLOR_LOW  = [0,   70,  120]  CP_COLOR_HIGH = [30,  140, 200]
```

---

## ДОПОМІЖНІ ПРОЕКТИ

- `~/l2bot/_l2cvbot/` — оригінальний l2cvbot з Linux портом (база для rdga1bot)
- `~/l2bot/l2cvbot_windows/` + `l2cvbot_windows_source.zip` — Windows build package

---

## ЗНАХІДКИ: L2S Reverse Engineering

### Таргетинг через /target макроси (РЕКОМЕНДОВАНО)
Гравець створює макроси `/target МобНейм` на хотбарі і вішає на клавіші F7-F11.
Бот натискає клавіші в ротації. Надійність ~100%.
УВАГА: цифрові клавіші (0-9) і нумпад можуть друкувати символи в чат L2 — використовувати F-keys!

### Loupe.exe — OCR (.sib формат)
`[ASCII][b1][b2][float threshold\0][256 bytes 16×16 template]`
Бази: `Имя_моба.sib`, `CPHPMP.sib`, `L2F3.sib`

### LOG.exe — читання через GDI (GetPixel/BitBlt), не хардкодовані адреси L2.

---

## СТАТУС ТЕСТУВАННЯ

### ✅ Варіант А (без гри) — ПРОЙДЕНО (2026-03-21)
```bash
./rdga1bot --no-tui --quick
# Виводить: Вікно "Lineage II" не знайдено! Чекаємо...
# SIGTERM → [Stats] Збережено в logs/stats_YYYY-MM-DD.json
```
- evdev UInput init — ОК
- XShm Capture init — ОК
- сигнальний хендлер → SaveToFile — ОК
- `setlinebuf(stdout)` в main() — вихід видний навіть без TTY

### ✅ Варіант В (повний FSM цикл) — ПРОЙДЕНО (2026-03-22)
```
IDLE→ATTACKING (watchdog 5s)→LOOTING→Вбивство#1→IDLE→ATTACKING→LOOTING→Вбивство#2
```
Баги виправлено під час тесту:
- **ESC self-trigger**: LOOTING надсилає ESC → main loop детектував його як "стоп". Фікс: `hands.IsReady()` guard перед `KeyboardKeyPressed(Escape)`
- **Core dump**: `cv::destroyAllWindows()` без попереднього `imshow` → crash. Фікс: викликати лише при `cfg.debug && !use_tui`

### ✅ Варіант Б (з грою) — ПРОЙДЕНО (2026-03-21)
```bash
printf "status\n" | ./rdga1bot --no-tui --quick
# [Brain] Ініціалізація...
# [STATE] IDLE -> ATTACKING   ← бот знайшов таргет і перейшов до атаки!
# State: ATTACKING | Kills: 0 | Deaths: 0 | Uptime: 00:00:00
```
- Eyes виявляє HP/MP/CP бари персонажа — ОК
- Eyes виявляє таргет HP бар — ОК
- Brain FSM переходить IDLE→ATTACKING при активному таргеті — ОК
- Бот реально відправляє evdev key events до Wine/L2 — ОК

---

## СТАТУС ГОТОВНОСТІ (2026-03-22)

### Підтверджено тестами (Flatpak Lutris + GE-Proton):
- XTest → Wine/L2 отримує клавіші ✓
- XShm захоплення → Eyes детектує HP/MP/CP бари персонажа ✓
- FSM IDLE→BUFFING→TARGETING→ATTACKING→LOOTING повний цикл ✓
- ALT+B ребаф (template matching score=99%) ✓ — виправлено MoveMouse→MoveMouseTo
- signal handlers (SIGINT/SIGTERM) → SaveToFile ✓
- ScrollLock = глобальна зупинка ✓
- DetectMyBars() нова архітектура ✓
- Каталоги template/ та tmp/ для PNG файлів ✓
- Eyes::DetectMinimap() ✓ — HSV red dots, ROI верхній правий кут
- Brain minimap rotate (dx>±20, limit 4) ✓
- Screen-Y NPC фільтр (cy<300 = далеко → ESC, max 5 відхилень) ✓
- Швидкий пропуск мертвих мобів (hp=0 → ESC 100мс) ✓
- Pokemon sweep (hp=0 + m_pokemon_targeted → Delay(1500) + ESC в TARGETING) ✓
- DetectMyBars() фікс: search_h 100→**80**, erode kernel (1,7)→**(3,7)** ✓
- **40-хвилинний тест (2026-03-22): 127 kills, 0 deaths, 4.3 kill/хв** ✓
- Оптимізації продуктивності (2026-03-23): Lazy HSV, XFlush batch, Focus cache, Blind spot relocation, Dashboard 10FPS ✓
- Approach re-target виправлено (2026-03-23): ESC перед F2, гейт `!m_first_attack`, без `return`, `m_approach_last_retarget = Now()` ✓
- TARGETING прискорено (2026-03-23): Send(150мс), чергування L/R ротація, WalkForward до моба ✓
- **100-хвилинний тест (2026-03-23): 1511 kills, 0 deaths, 15.1 kill/хв, 12 watchdog, 158 pokemon** ✓
- **3-годинний тест (2026-03-23): 1940 kills, 0 deaths, 11.0 kill/хв, uptime 2:56:25** ✓
- Timestamps в Brain::Log() (2026-03-23): всі лог-рядки мають `[HH:MM:SS]` префікс ✓
- Buff loop фікс (2026-03-23): interrupt by target тепер додає 30с cooldown (`m_last_buff = Now() - (buff_interval - 30)`) ✓
- Silent freeze detection (2026-03-23): `m_detect_me_fail_count` → WARNING через 3с, повтор кожні 30с ✓
- `farm.sh` — скрипт для тривалого фарму з логом у `logs/session_YYYYMMDD_HHMMSS.log` ✓
- Screen-Y фільтр tuned (2026-03-23): поріг 300→**450**, макс відхилень 5→**10** ✓
- **Screen-Y фільтр retuned (2026-03-24)**: 450→**300**, макс відхилень 10→**3** — пустеля 450 була надто агресивна для підземель; 3 відхилення → приймаємо будь-який таргет ✓
- Approach re-target tuned (2026-03-23): затримка 0.6→**1.0с**, entry HP tracking (`m_approach_entry_hp`) — не перемикає якщо моб вже отримав удар ✓
- Heartbeat (2026-03-23): кожні 5с `[HB] State=X HP=Y ready=Z buff_in=Wс` ✓
- IsReady warning (2026-03-23): WARNING після 20 тіків IsReady=false в TARGETING ✓
- **Dead target freeze FIX (2026-03-23)**: `m_dead_target_esc_count` — лог кожного ESC для hp=0, після 5 спроб fallthrough до F2 замість return → **усунуто головне зависання після бафу** ✓
  - Причина: `DetectTarget()` повертала hp=0 (стара ціль) → бот мовчки слав ESC нескінченно → жодного логу, `m_not_ready_count` ніколи не досягав 20 (скидався кожен 2-й тік)
- **Approach re-target guard (2026-03-23)**: `m_approach_entry_hp >= 90` + `hp >= entry_hp-5` — ретаргет лише свіжих мобів, не перемикає якщо моб вже пошкоджений ✓
- **Macro fallback threshold (2026-03-23)**: `/target` макроси (F7-F11) тільки після 10+ невдалих F2 (`kMacroFallbackAfter=10`) — щоб не перебивати /nexttarget ✓
- **TargetStatusWnd динамічна позиція (2026-03-23)**: читається з `WindowsInfo.ini` через `[Vision] WindowsInfoPath`; `eyes.SetTargetWnd()` в `ApplyConfig()` ✓
- **Approach re-target: фільтр dying mobs (2026-03-23)**: після ESC+F2, якщо новий моб HP<30% → ESC + не рахуємо ретаргет (`m_approach_retarget_count--`) → шукаємо кращого. Запобігає вибору мобів що добиваються іншими гравцями ✓
  - Фіксує: при HP≤25% fill bar не досягав хардкодованого x=598 (реальна posX=539) → hp=0 → dead target → пропуск моба
- **DetectTargetHPDirect V threshold: 80→30 (2026-03-23)**: темний бар (V≈47-78) тепер детектується коректно ✓
- **dead_target_esc_count reset fix (2026-03-24)**: після fallthrough (>5 ESC) лічильник скидається до 0 → наступний цикл починається з ×1 (раніше йшло ×6,×7,... нескінченно) ✓
- **[Pokemon] замість [Spoil] в лозі (2026-03-24)**: `Pokemon sweep` і `Pokemon макрос` — не плутати з Spoiler класом ✓
- **Auto-calibration m_target_wnd_x + false positive guard (2026-03-24)**: при fill=0 сканує рядок y=27..32. Guard: `scan_best>=8` + `|new_x-base|<=150px` — без guard стрибки 559→415→700→349 від buff icons. `m_target_wnd_x_base` незмінний. ✓
- **Approach re-target: поріг dying mobs 30%→15% (2026-03-24)**: 30% — на активних спотах ALL моби 15-29% → 7 скасувань за 4хв. 15% — правильний баланс. ✓
- **BuffInterval 900→600с (2026-03-24)** ✓
- **GitHub repo (2026-03-24)**: `git@github.com:rdga1bot/rdga1bot.git` (приватний, GPL v3) ✓
- **Debug PNG save прибрано (2026-03-24)**: `tmp/dead_target_debug.png` → 120-160ms slow ticks. Видалено. ✓
- **Лог косметика (2026-03-24)**: `hp=-1%` → `hp=?`, autocal лог: `x=old → new` ✓
- **Buff fallback координати (2026-03-24)**: `BuffTabX=773, BuffTabY=297, BuffProfileX=841, BuffProfileY=252`. Були 2465/2532 (multi-monitor) → кліки поза екраном → баф не застосовувався + 600с чекання. ✓
- **Buff fallback → retry 120с (2026-03-24)**: `m_buff_tab_fallback` flag → якщо шаблон не знайдено → retry через 120с замість 600с. ✓
- **Тести 2026-03-24**: 222 kills/20хв (11.1/хв), Kill(hp=1-5%) ✓, 0 deaths ✓
- **Kill threshold 5%→2% (2026-03-24)**: 5% → хибні LOOTING при живих мобах 1-3% (1px = 0.66%, бар 152px). 2% — безпечний мінімум: моб з 1px залишку = hp≈1% → ще атакується. ✓
- **farm.sh: --quick fix (2026-03-24)**: `./rdga1bot --no-tui` без `--quick` → setup wizard читав stdin=/dev/null → ESC → зупинка. Додано `--quick`. ✓
- **Pokemon macro: 1 раз на початку циклу (2026-03-24)**: Було `% 10 == 5` → 2+ sweeps/цикл × 1500мс = +3с. Тепер `attempt==1 + flag m_pokemon_macro_fired` — один раз, відразу. Flag скидається в EnterState(Targeting). ✓
- **Dead-target loop → macro switch (2026-03-24)**: Після 3 ESC-fallthrough циклів (`m_dead_cycles_total>=3`) → одразу `/target Name` macro (не чекати 10 спроб). `/target Name` не вибирає мертвих. `m_dead_cycles_total` скидається при живому/відсутньому таргеті. ✓
- **Buff retry 30с→10с (2026-03-24)**: на активних спотах моби кожні 5-10с → бот ніколи не міг зібрати 30с паузи → баф не відбувався. Знижено до 10с. ✓
- **Minimap purple dot detection (2026-03-24)**: `MinimapDot::selected` — фіолетовий ореол вибраного моба після `/target`. Brain ротується до selected замість nearest. Деградує gracefully якщо пульсація off. ✓
- **Log move semantics (2026-03-24)**: `m_log_callback(std::move(line))` — уникає зайвого string copy до Dashboard. ✓
- **Buff score=32%→retry→99% підтверджено (2026-03-24)**: логи тесту: retry 1/3 → score=99% → успішно. ✓
- **Autocal одноразова (2026-03-24)**: x=598→559 при першому таргеті, далі жодних стрибків (guard scan_best≥8 + ±150px від base). ✓
- **Screen-Y фільтр retuned (2026-03-24)**: 450→**300**, макс відхилень 10→**3** — пустеля 450 була надто агресивна для підземель; 3 відхилення → приймаємо будь-який таргет ✓
- **Kill threshold 5%→2% (2026-03-24)**: 5% → хибні LOOTING при живих мобах 1-3% (1px = 0.66%, бар 152px). 2% — безпечний мінімум: моб з 1px залишку = hp≈1% → ще атакується. ✓
- **farm.sh: --quick fix (2026-03-24)**: `./rdga1bot --no-tui` без `--quick` → setup wizard читав stdin=/dev/null → ESC → зупинка. Додано `--quick`. ✓
- **Screen-Y та MaxFarRejects → .ini (2026-03-24)**: `NearbyYThreshold` (дефолт 200) і `MaxFarRejects` (дефолт 5) у `[Targeting]`. `0` = вимкнено. Для підземель 0-200, для пустелі 350-450. ✓
- **Minimap S поріг 180→100, area_min 4.0→2.0 (2026-03-24)**: в підземеллях точки мобів менші і менш насичені. Нові значення сумісні з обома локаціями. ✓
- **LootEnabled / BuffEnabled (2026-03-28)**: master on/off перемикачі для LOOTING і BUFFING. `false` = відповідний стан повністю пропускається. ✓
- **Конфігуровані Navigation параметри (2026-03-28)**: `StuckDetection`, `WallDetection`, `FlowDetection`, `StuckThreshold` у `[Navigation]` ini. ✓
- **Dashboard порядок барів виправлено (2026-03-28)**: CP/HP/MP (top→bottom) відповідає грі. Було HP/MP/CP (неправильно). ✓
- **Dead-target debounce в TARGETING (2026-03-28)**: 1-тік очікування перед першим ESC при hp=0. Причина: гра рендерить TargetStatusWnd UI ~150-200мс після F2. Перший тік після таргетингу може показати hp=0 (race condition) → ESC видаляв живу ціль. Тепер: ×1 = чекаємо, ×2+ = ESC. ✓
- **NearbyYThreshold = 0 для підземель (2026-03-28)**: Screen-Y фільтр вимкнено в .ini. В підземеллях моби часто з'являються при cy < 200 (верхня частина екрану через кут камери) → фільтр відхиляв близьких мобів. Для пустелі/відкритих зон: 350-450. ✓
- **Minimap H range fix (2026-03-29)**: H upper bound 10→**20** — виміряно H=10-19 дає 1417px точок (мобів) vs H=0-9 лише 35px у підземеллях. Фікс усунув "мінімапа порожня" в підземеллях. ✓
- **Progressive rotation (2026-03-29)**: `m_nav_stuck_recoveries` лічильник → кожні 2 застрягання +450мс (900→1350→1800мс max), чергує R/L незалежно від `m_macro_attempts`. Після успішного руху скидається до 0. ✓
- **dy-based WalkForward (2026-03-29)**: якщо моб на мінімапі по центру (`|dx|≤20`) і спереду (`dy<-15`) → WalkForward(700мс) одразу (без чекання fallback кожні 4 спроби). ✓
- **Patrol system (2026-03-29)**: `[Patrol]` секція в .ini — `PatrolPath = F2000,R500,...` — тайм-крокова навігація коли мінімапа порожня `PatrolTriggerAttempts` спроб. Директиви: F/B/L/R + мілісекунди. ✓
- **MemReader (2026-03-29)**: `src/MemReader.cpp` — читання HP/MP/CP/XYZ з Wine L2 процесу через `process_vm_readv` (без root). `[MemReader]` секція в .ini з hex offsets. При `Enabled=true` значення з пам'яті перекривають OpenCV детекцію. ✓
- **GetMinimapFlow() optical flow (2026-03-29)**: Lucas-Kanade на мінімапі ROI (185×165px) замість повного кадру. `goodFeaturesToTrack` + `calcOpticalFlowPyrLK` між кадрами мінімапи. flow<0.3 протягом 2с при наявних мобах на мінімапі → escape rotation. Надійніше ніж frame diff центру (підземелля мають одноманітні текстури). `FlowDetection=true` в .ini. ✓
- **Buff cooldown у Process() (2026-03-29)**: cooldown перевіряється в `Process()` до `EnterState(Buffing)` — бот не входить в BUFFING якщо є активний бій (nещодавній kill). Прибрано blocking 1с×20с wait з `HandleBuffing()`. Більше ніяких `[Buffs] Чекаємо cooldown 19с...` з мобами поряд. ✓
- **Buff trigger: SecsSince→m_macro_attempts (2026-03-29)**: `SecsSince(last_kill_time) >= cooldown` ніколи не виконувалась на активних спотах (kills кожні 2-5с, cooldown 10с → постійний reset). Замінено на `m_macro_attempts >= (int)(cooldown * 1000/150)` — proxy "N consecutive targeting attempts without mob". m_macro_attempts=0 при EnterState(Targeting), тому вимірює реальний час без мобів. ✓
- **Flow-stuck false positives FIX (2026-03-29)**: GetMinimapFlow() повертає ~0 коли персонаж стоїть (звичайний таргетинг F2) → окремий 2с таймер тригерив `[NAV] Flow-stuck: flow=0.00 2с` 24+ рази за сесію. Прибрано окремий блок (lines 504-539); GetMinimapFlow() тепер тільки всередині `m_nav_prev_was_walk` блоку як третій сигнал руху. ✓
- **m_attack_was_unreachable (2026-03-29)**: якщо HP-stable (5с без пошкоджень) → `m_attack_was_unreachable=true` → наступний TARGETING відкладає макроси до attempt 15 (замість 2) → навігація має час вийти з кімнати. Скидається коли мінімапа показує мобів. ✓

### Потребує уваги:
- **dead_target ×1..6**: нормально — гра re-selects труп після ESC, 5-6 циклів до despawn (5-10с)
- **Buff template score 37%**: якщо ALT+B вікно не відкривається → fallback + retry 120с. Якщо регулярно → перезняти `template/buff_tab.png`

## ПОТОЧНИЙ СТАН КЛАВІШ

| Клавіша | Призначення |
|---------|-------------|
| F1 | Атака (AttackKeys) |
| F2 | /nexttarget (NextTargetKey) — основний таргетинг |
| F3 | Pokemon macro: `/target Pokemon` + `/useshortcut 2 5` (кожні 10 спроб TARGETING) |
| F7–F11 | Резервні /target макроси (MacroKeys) |
| F12 | Калібровка → tmp/calibrate*.png + HSV suggest |
| PrintScreen | tmp/shot.png |
| ScrollLock | Зупинка бота (глобально) |
| HP/MP/CP зілля | ВИМКНЕНО (гра auto-potion) |
| Лут | ВИМКНЕНО (гра auto-loot) |

## НАСТУПНІ КРОКИ

1. **Тест Flow Detection**: у логах шукати `[NAV] Flow-stuck: flow=0.XX 2с → WalkBack + Rotate` — підтверджує що GetMinimapFlow() спрацював. Якщо не з'являється при застряганні → flow > 0.3 (текстура мінімапи нестабільна) або мінімапа порожня.
2. **Тест buff cooldown fix**: переконатись що `[Buffs] Чекаємо cooldown Nс...` більше не з'являється. Баф має старту вати лише коли `buff_in=0с` в `[HB]` рядку і між TARGETING спробами є пауза >20с.
3. **Тест unreachable mob fix**: якщо `[ATTACKING] HP стабільний 5с` → наступний TARGETING лог не повинен мати `[target macro]` в перших 15 спробах — тільки F2 + навігація.
4. **MemReader offsets**: якщо хочемо точні HP/MP/CP з пам'яті — знайти offsets через Cheat Engine під Wine, вписати в `[MemReader]` + `Enabled = true`.
5. **Patrol налаштування**: для прямолінійних коридорів: `PatrolEnabled = true`, `PatrolPath = F2000,R700,F1500,L700`. Тестувати поки мінімапа порожня.
6. **Тривалий фарм** — бот стабільний. Моніторити логи на `[NAV] Flow-stuck` і `[ATTACKING] HP стабільний`.
