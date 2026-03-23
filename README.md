# rdga1bot — ElmoreLab Farm Bot

C++ бот для автоматизації фарму в Lineage II на ElmoreLab (Arch Linux, Wine/Lutris).

## Можливості

- **Таргетинг через макроси `/target`** — надійніше за піксельну детекцію імен мобів
- **Ротація атак** — кілька скілів по черзі
- **Spoil/Sweep** для Spoiler класу (автоматично за Class=Spoiler)
- **Self-buff таймер** — автоматичний ребаф через заданий інтервал
- **HP/MP/CP моніторинг** — автоматичне вживання зілль з кулдауном 5с
- **Auto-approach** — крок вперед якщо HP цілі не падає >6с (out-of-range)
- **Прогресивний пошук** — чим довше немає мобів, тим далі ходить
- **Dead detection** — Enter → чекати 20с → grace period 30с після respawn
- **Telegram сповіщення** — смерть, статистика (через curl, без libcurl)
- **Інтерактивне TUI** — кольорове меню при першому запуску
- **INI конфігурація** — все без перекомпіляції
- **30-60 FPS** — нативний C++, XShm screen capture, evdev/UInput input

## Швидкий старт

```bash
# 1. Зібрати
./build.sh

# 2. Підготувати гру:
#    - Відкрити Lineage II (windowed mode)
#    - Створити макроси /target МобНейм1, /target МобНейм2 в грі
#    - Повісити макроси на хотбар: слоти 2,3,4,5,6
#    - Повісити: атаку F1, лут F5, HP зілля F6, MP F7, CP F8

# 3. Запустити (перший раз — інтерактивне налаштування)
./launch.sh

# 4. Наступні рази (швидкий старт з rdga1bot.ini)
./rdga1bot --quick
# або
./launch.sh   # автоматично --quick якщо rdga1bot.ini існує
```

## Конфігурація (rdga1bot.ini)

Створюється автоматично через TUI при першому запуску.
Можна редагувати вручну текстовим редактором.

```ini
[General]
WindowTitle = Lineage II
Debug = true

[Character]
Class = Spoiler  # Mage, Archer, Spoiler

[Targeting]
MacroKeys = 2,3,4,5,6  # хотбар-слоти макросів /target

[Attack]
AttackKeys = F1
AttackWait = 0.5

[Loot]
LootKey = F5
LootCount = 10

[Potions]
HP_Key = F6
HP_Threshold = 70
MP_Key = F7
MP_Threshold = 40

[Telegram]
BotToken =
ChatID =
```

## Клавіші під час роботи

| Клавіша | Дія |
|---------|-----|
| ESC | Зупинити бот |
| PrintScreen | Зберегти скріншот (`shot.png`) |
| Space | Скинути позиції HP/MP/CP барів |
| F12 | Зберегти калібрувальні зображення (`calibrate_*.png`) |

## Стейт-машина

```
IDLE → TARGETING → ATTACKING → LOOTING → IDLE
                      ↓
                    DEAD → (Enter → 20s wait → respawn grace) → IDLE
                      ↓
                  BUFFING → IDLE
```

## Залежності

```bash
sudo pacman -S opencv gcc x11 libxtst libxext curl
```

- `opencv` — screen analysis
- `x11`, `libxtst`, `libxext` — X11, XTest, XShm
- `/dev/uinput` — kernel-level keyboard input (evdev)
- `curl` — Telegram сповіщення (опціонально)

## Архітектура

| Файл | Роль |
|------|------|
| `Brain.cpp` | Стейт-машина (IDLE/TARGETING/ATTACKING/LOOTING/DEAD/BUFFING) |
| `Eyes.cpp` | OpenCV детекція HP/MP/CP барів, target HP |
| `Hands.h` | Дії: макроси, атака, баф, рух, зілля |
| `Config.cpp` | INI парсер + інтерактивний TUI |
| `Capture_Linux.cpp` | XShm screen capture (~30fps) |
| `Intercept_Linux.cpp` | evdev UInput keyboard, XTest mouse |
| `Window_Linux.cpp` | X11 window finding |
| `Notify.cpp` | Telegram через fork+curl |
| `Stats.cpp` | Статистика сесії, JSON лог |
