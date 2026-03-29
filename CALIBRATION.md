# KnownList Calibration Guide

Покрокова інструкція з налаштування KnownList сканера для читання об'єктів
з пам'яті Wine/L2 процесу без Cheat Engine.

---

## Що таке KnownList

KnownList — це масив вказівників на об'єкти (моби, гравці, предмети) поблизу
персонажа, який L2 клієнт зберігає в пам'яті. Зчитуючи його, бот може:

- Точно знати HP кожного моба (float, без OpenCV)
- Знати чи моб мертвий миттєво (не чекати 8 тіків debounce)
- Бачити предмети на землі для підбору
- Знайти найближчого живого моба без детекції мінімапи

---

## Передумови

1. Гравець запущений в грі (не в кат-сцені, не в меню).
2. Поруч є хоча б 1 моб (для верифікації KnownList).
3. `[MemReader]` **НЕ потрібен** — KnownList STANDALONE.

---

## Крок 1: Включити KnownList в .ini

```ini
[KnownList]
Enabled = true
AutoScan = true
OffsetsFile = offsets.json
MaxRange = 1200
```

При `AutoScan = true` бот автоматично запустить `OffsetScanner::blindScan()`
кожні 5с до першого успіху. Координати гравця **не потрібні** — scan суто
структурний: перевіряє `base+0x120` → масив об'єктів з L2-координатами
в межах світу (±327k XY / ±16k Z).

Знайдені offsets зберігаються в `offsets.json` і завантажуються при наступних
запусках (без повторного сканування).

---

## Крок 2: Автоматичний скан (рекомендовано)

1. Запустіть бот: `./rdga1bot --quick`
2. Зайдіть в гру, станьте поруч з мобами.
3. У stderr/логах шукайте:
   ```
   [KnownList] blind scan спроба #1
   [OffsetScanner] blindScan: N регіонів, M MB
   [OffsetScanner] blindScan: PlayerBase=0xXXXXXXXX XYZ=(X,Y,Z) KnownCount=N
   [KnownList] PlayerBase=0x... WorldState активовано
   ```
4. Якщо знайдено — `offsets.json` зберігається автоматично.

**Якщо PlayerBase не знайдено** (`blindScan: PlayerBase не знайдено`):
- Скан повторюється кожні 5с — зачекайте 30с
- Переконайтесь що L2 клієнт запущений і персонаж в грі (не в меню)
- Поруч є хоча б 1 живий моб (порожній спот → KnownCount може бути 0)
- Якщо постійно не знаходить → спробуйте ручне калібрування (Крок 3)

---

## Крок 3: Ручне калібрування (якщо AutoScan не спрацював)

### 3a. Знайти PlayerBase вручну

Якщо `blindScan()` не знаходить PlayerBase (рідкісний випадок — наприклад
нестандартний клієнт або нетипові offsets), можна використати координатний метод:

```bash
# Дізнатись PID l2.exe:
cat /proc/$(pgrep -af l2.exe | awk '{print $1}')/maps | head -5
```

Отримайте поточні координати гравця через `/loc` команду в грі або з логів
L2 клієнта, потім використайте `findPlayerBase()`:

```cpp
OffsetScanner scanner(pid);
// x, y, z — точні поточні координати гравця (стоїть нерухомо)
uintptr_t base = scanner.findPlayerBase(x_coord, y_coord, z_coord, 2.0f);
```

### 3b. Знайти KnownList offset

Коли PlayerBase знайдено — поставте поруч 1 моба і запустіть:

```cpp
scanner.findKnownListOffset(base, mob_x, mob_y, 20.0f);
```

Вивід покаже кандидати: `offset=0xXXX ptr=0x... X=1234 Y=5678`.
Знайдений offset запишеться автоматично в `scanner.knownListOff`.

### 3c. Калібрування offsets об'єкту (якщо моби не читаються)

Якщо тип/HP мобів невірні для вашого клієнта:

```cpp
// objPtr — адреса першого об'єкту з KnownList
scanner.calibrateObjectOffsets(objPtr, mob_x, mob_y, mob_z, 5.0f);
```

Вивід покаже зміщення де знайдено X/Y/Z.
Відповідно оновіть `scanner.objXOff / objYOff / objZOff`.

### 3d. Зберегти результат

```cpp
scanner.saveOffsets("offsets.json");
```

---

## Крок 4: Перевірка

Після активації в логах бота мають з'являтись:
```
[ATTACKING] [KnownList] Таргет мертвий → LOOTING
```
замість звичайного `NoTarget ×8` або `Kill(hp=0%)`.

Якщо `[KnownList]` рядки не з'являються — KnownList не активований або
`m_player_base = 0` (PlayerBase не знайдено).

---

## Структура offsets.json

```json
{
  "OFF_KNOWN_LIST": 288,
  "OFF_KNOWN_COUNT": 292,
  "OFF_OBJ_TYPE": 24,
  "OFF_OBJ_X": 36,
  "OFF_OBJ_Y": 40,
  "OFF_OBJ_Z": 44,
  "OFF_CHAR_HP": 500,
  "OFF_CHAR_HP_MAX": 504,
  "OFF_CHAR_IS_DEAD": 528
}
```

Значення у decimal (не hex). `OffsetsFile = offsets.json` в .ini вказує шлях.

---

## Дефолтні offsets (HF client)

| Поле             | Hex    | Decimal |
|-----------------|--------|---------|
| OFF_KNOWN_LIST  | 0x120  | 288     |
| OFF_KNOWN_COUNT | 0x124  | 292     |
| OFF_OBJ_TYPE    | 0x18   | 24      |
| OFF_OBJ_X       | 0x24   | 36      |
| OFF_OBJ_Y       | 0x28   | 40      |
| OFF_OBJ_Z       | 0x2C   | 44      |
| OFF_CHAR_HP     | 0x1F4  | 500     |
| OFF_CHAR_HP_MAX | 0x1F8  | 504     |
| OFF_CHAR_IS_DEAD| 0x210  | 528     |

⚠ Ці значення для HF/Kamael клієнта. Для інших chronicle можуть відрізнятись.
Використовуйте `OffsetScanner::blindScan()` або `calibrateObjectOffsets()` для верифікації.

---

## Обмеження

- `process_vm_readv` без root потребує того самого UID що і Wine/L2 процес.
- KnownList може містити до ~2000 об'єктів. Читання ~100мс при 2000 obj —
  в нормальних умовах 50-200 мобів на спот (~5-10мс).
- PlayerBase змінюється при перелогіні/телепортації — перезапустіть бот
  або зачекайте 5с (blindScan() запускається автоматично при `HasPlayerBase()=false`).
- `blindScan()` сканує ~64MB heap ділянок (~2-10с залежно від машини) — виконується
  лише один раз до першого успіху, потім кешується в `offsets.json`.
