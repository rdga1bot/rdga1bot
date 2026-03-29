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

1. `[MemReader]` налаштований і `Enabled = true` — бот повинен вже зчитувати
   HP/MP/CP гравця через `process_vm_readv`.
2. Гравець стоїть у грі (не в кат-сцені, не в меню).
3. Поруч є хоча б 1 моб (для верифікації KnownList offset).

---

## Крок 1: Включити KnownList в .ini

```ini
[KnownList]
Enabled = true
AutoScan = true
OffsetsFile = offsets.json
MaxRange = 1200
```

При `AutoScan = true` бот автоматично запустить `OffsetScanner::findPlayerBase()`
при першому тіку з валідними координатами гравця (потрібен `[MemReader] Enabled = true`
і правильні PosX/Y/Z offsets).

Знайдені offsets зберігаються в `offsets.json` і завантажуються при наступних запусках
(без повторного сканування).

---

## Крок 2: Автоматичний скан (рекомендовано)

1. Запустіть бот: `./rdga1bot --quick`
2. Зайдіть в гру, станьте нерухомо поруч з мобами.
3. У stderr/логах шукайте:
   ```
   [OffsetScanner] findPlayerBase: N кандидатів
   [OffsetScanner] PlayerBase=0xXXXXXXXX (KnownList ptr=0xYYYYYYYY)
   [KnownList] PlayerBase=0x... WorldState активовано
   ```
4. Якщо знайдено — `offsets.json` зберігається автоматично.

**Якщо PlayerBase не знайдено:**
- Переконайтесь що `[MemReader] PosX_Offset/Y/Z` виставлені правильно
  (бот повинен зчитувати реальні X/Y/Z з пам'яті)
- Спробуйте `AutoScan = false` і ручне калібрування (Крок 3)

---

## Крок 3: Ручне калібрування (якщо AutoScan не спрацював)

### 3a. Знайти PlayerBase

Відкрийте `/proc/<pid>/maps` (де pid = PID l2.exe):
```bash
cat /proc/$(pgrep -f l2.exe)/maps | head -20
```

Запустіть OffsetScanner вручну (через тимчасову тестову програму або
інтерактивний режим бота):

```cpp
OffsetScanner scanner(pid);
// x, y, z — поточні координати з /nexttarget або з логів гри
uintptr_t base = scanner.findPlayerBase(x_coord, y_coord, z_coord, 2.0f);
```

Координати можна підглянути у системному логу L2 клієнта або через
`/loc` команду в грі (якщо сервер підтримує).

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
Використовуйте `OffsetScanner` або Cheat Engine для верифікації.

---

## Обмеження

- `process_vm_readv` без root потребує того самого UID що і Wine/L2 процес.
- KnownList може містити до ~2000 об'єктів. Читання ~100мс при 2000 obj —
  в нормальних умовах 50-200 мобів на спот (~5-10мс).
- PlayerBase змінюється при перелогіні/телепортації — `kl_scan_done` reset
  при перезапуску бота автоматично.
