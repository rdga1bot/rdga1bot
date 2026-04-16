"""
log_tailer.py — tail-f парсер session_*.log файлів rdga1bot.

Реальні теги з логів:
  [HH:MM:SS] [BT] Attack avg=444µs
  [HH:MM:SS] [HB] OBJ=Attack HP=70 MP=55 CP=95 ready=Y buff_in=895с
  [HH:MM:SS] [LOOTING] Вбивство #N
  [HH:MM:SS] [DEAD] ...
  [HH:MM:SS] [TARGETING] Спроба N — ...
  [HH:MM:SS] [MAP] ...
  [HH:MM:SS] [BLACKLIST] ...
  [HH:MM:SS] [POTION] ...
  [HH:MM:SS] [Buffs] ...
  [HH:MM:SS] [STATE] X -> Y
  [HH:MM:SS] [OBJ] Enter: X
  [HH:MM:SS] [NAV] ...
"""

import re
import os
import time
import asyncio
from collections import deque
from datetime import datetime, date
from typing import Optional, Callable, List

# ---------------------------------------------------------------------------
# Regex-паттерни для реальних рядків з session_*.log
# ---------------------------------------------------------------------------

# Timestamp prefix: [HH:MM:SS]  — може бути відсутній (старі рядки)
_TS_PREFIX = r'(?:\[(\d{2}:\d{2}:\d{2})\]\s*)?'

LOG_PATTERNS: List[tuple] = [
    # [BT] Attack avg=444µs
    (re.compile(_TS_PREFIX + r'\[BT\]\s+(\w+)\s+avg=(\d+)µs'),
     'bt_tick', ['branch', 'avg_us']),

    # [BT] BotBehaviorTree: N вузлів
    (re.compile(_TS_PREFIX + r'\[BT\]\s+BotBehaviorTree:\s+(\d+)'),
     'bt_init', ['node_count']),

    # [HB] OBJ=Attack HP=70 MP=55 CP=95 ready=Y buff_in=895с
    (re.compile(_TS_PREFIX + r'\[HB\]\s+OBJ=(\S+)\s+HP=(\d+)\s+MP=(\d+)\s+CP=(\d+)'),
     'heartbeat', ['obj', 'hp', 'mp', 'cp']),

    # [LOOTING] Вбивство #N
    (re.compile(_TS_PREFIX + r'\[LOOTING\]\s+Вбивство\s+#(\d+)'),
     'kill', ['kill_num']),

    # [DEAD] anything
    (re.compile(_TS_PREFIX + r'\[DEAD\](.*)'),
     'dead', ['detail']),

    # [TARGETING] Спроба N — ...
    (re.compile(_TS_PREFIX + r'\[TARGETING\]\s+Спроба\s+(\d+)'),
     'targeting_attempt', ['attempt']),

    # [TARGETING] Довгий пошук ×N ...
    (re.compile(_TS_PREFIX + r'\[TARGETING\]\s+Довгий\s+пошук\s+[×x](\d+)(.*)'),
     'targeting_long', ['count', 'detail']),

    # [BLACKLIST] mob_id=N (legacy) or [BLACKLIST] Моб ID=N
    (re.compile(_TS_PREFIX + r'\[BLACKLIST\]\s+(?:mob_id=|Моб\s+ID=)(\d+)'),
     'blacklist', ['mob_id']),

    # [OBJ] Enter: Attack
    (re.compile(_TS_PREFIX + r'\[OBJ\]\s+Enter:\s+(\w+)(?:\s+\[([^\]]+)\])?'),
     'obj_enter', ['objective', 'context']),

    # [STATE] IDLE -> ATTACKING
    (re.compile(_TS_PREFIX + r'\[STATE\]\s+(\w+)\s*->\s*(\w+)'),
     'state_change', ['from_state', 'to_state']),

    # [MAP] Найближчий моб ліворуч (dx=-30, rot=1) → RotateLeft
    (re.compile(_TS_PREFIX + r'\[MAP\]\s+(.+?)\(dx=(-?\d+).*?\)'),
     'minimap', ['direction', 'dx']),

    # [NAV] Не рухаємось після WalkForward ×N flow=X.XX
    (re.compile(_TS_PREFIX + r'\[NAV\]\s+(.+)'),
     'nav', ['detail']),

    # [KL-HP] nearest dist=150 hpMax=0 hpAbs=2847 ocr=78
    (re.compile(_TS_PREFIX + r'\[KL-HP\]\s+nearest\s+dist=(\d+)\s+hpMax=(\d+)\s+hpAbs=(\d+)\s+ocr=(\d+)'),
     'kl_hp', ['dist', 'hp_max', 'hp_abs', 'ocr']),

    # [KL-HP] nearest dist=150 hpMax=500 hp%=78 ocr=78
    (re.compile(_TS_PREFIX + r'\[KL-HP\]\s+nearest\s+dist=(\d+)\s+hpMax=(\d+)\s+hp%=(-?\d+)\s+ocr=(\d+)'),
     'kl_hp_pct', ['dist', 'hp_max', 'hp_pct', 'ocr']),

    # [POTION] HP N% < N% → вживаємо
    (re.compile(_TS_PREFIX + r'\[POTION\]\s+(HP|MP|CP)\s+(\d+)%'),
     'potion', ['potion_type', 'current_pct']),

    # [Buffs] Завершено, наступний баф через Nс
    (re.compile(_TS_PREFIX + r'\[Buffs\]\s+Завершено'),
     'buff_done', []),

    # [Buffs] ALT+B / template matching
    (re.compile(_TS_PREFIX + r'\[Buffs\]\s+(.+)'),
     'buff_event', ['detail']),

    # [PERF] Повільний тік: Nмс
    (re.compile(_TS_PREFIX + r'\[PERF\]\s+Повільний\s+тік:\s+(\d+)мс'),
     'slow_tick', ['ms']),

    # [KnownList] mobs=N alive=N items=N
    (re.compile(_TS_PREFIX + r'\[KnownList\]\s+mobs=(\d+)\s+alive=(\d+)\s+items=(\d+)'),
     'knownlist', ['mobs', 'alive', 'items']),

    # [Pokemon] макрос
    (re.compile(_TS_PREFIX + r'\[Pokemon\]\s+(.+)'),
     'pokemon', ['detail']),
]


def _parse_time(ts_str: Optional[str], ref_date: Optional[date] = None) -> Optional[datetime]:
    """Конвертує HH:MM:SS рядок + референтну дату в datetime."""
    if not ts_str:
        return None
    ref = ref_date or date.today()
    try:
        t = datetime.strptime(ts_str, "%H:%M:%S").time()
        return datetime.combine(ref, t)
    except ValueError:
        return None


def parse_line(line: str, ref_date: Optional[date] = None) -> Optional[dict]:
    """
    Парсить один рядок логу в structured event dict.
    Повертає None якщо рядок не розпізнано.
    """
    line = line.rstrip('\n')
    if not line:
        return None

    for pattern, event_type, fields in LOG_PATTERNS:
        m = pattern.search(line)
        if m:
            groups = m.groups()
            ts_str = groups[0]  # перша група — завжди timestamp або None
            values = groups[1:]  # решта — поля події

            event = {
                "type": event_type,
                "raw": line,
                "ts_str": ts_str,
                "timestamp": _parse_time(ts_str, ref_date),
            }
            # Заповнюємо поля з регексу
            for i, field in enumerate(fields):
                if i < len(values):
                    val = values[i]
                    # Числові поля — автоконверсія
                    if field in ('avg_us', 'kill_num', 'attempt', 'count',
                                 'ms', 'mobs', 'alive', 'items', 'node_count',
                                 'mob_id', 'dx', 'current_pct', 'hp', 'mp', 'cp',
                                 'dist', 'hp_max', 'hp_abs', 'hp_pct', 'ocr'):
                        try:
                            val = int(val)
                        except (TypeError, ValueError):
                            pass
                    event[field] = val
                else:
                    event[field] = None

            return event

    return None


def parse_file(path: str) -> List[dict]:
    """Парсить весь файл логу, повертає список подій."""
    events = []
    # Спробуємо витягти дату з імені файлу session_YYYYMMDD_...log
    ref_date = None
    basename = os.path.basename(path)
    m = re.search(r'session_(\d{4})(\d{2})(\d{2})', basename)
    if m:
        try:
            ref_date = date(int(m.group(1)), int(m.group(2)), int(m.group(3)))
        except ValueError:
            pass

    with open(path, 'r', encoding='utf-8', errors='replace') as fh:
        for line in fh:
            ev = parse_line(line, ref_date)
            if ev:
                events.append(ev)
    return events


class LogTailer:
    """
    Асинхронний tail-f читач session log файлів.
    Підтримує як поточний файл (найновіший session_*.log),
    так і replay конкретного файлу.
    """

    def __init__(self, log_dir: str = "logs", queue: Optional[asyncio.Queue] = None):
        self.log_dir = log_dir
        self.queue = queue
        self._file = None
        self._path = None
        self._ref_date = None
        self._pos = 0
        self._running = False

    def _find_latest_log(self) -> Optional[str]:
        """Знаходить найновіший session_*.log файл."""
        logs_dir = self.log_dir
        candidates = []
        for fname in os.listdir(logs_dir):
            if fname.startswith("session_") and fname.endswith(".log"):
                candidates.append(os.path.join(logs_dir, fname))
        if not candidates:
            return None
        return max(candidates, key=os.path.getmtime)

    def open(self, path: Optional[str] = None, seek_end: bool = True):
        """Відкриває файл для читання (seek_end=True → читаємо тільки нові рядки)."""
        if path is None:
            path = self._find_latest_log()
        if path is None:
            return False

        self._path = path
        basename = os.path.basename(path)
        m = re.search(r'session_(\d{4})(\d{2})(\d{2})', basename)
        if m:
            try:
                self._ref_date = date(int(m.group(1)), int(m.group(2)), int(m.group(3)))
            except ValueError:
                self._ref_date = date.today()
        else:
            self._ref_date = date.today()

        self._file = open(path, 'r', encoding='utf-8', errors='replace')
        if seek_end:
            self._file.seek(0, 2)  # seek to end
        self._pos = self._file.tell()
        return True

    def drain(self) -> List[dict]:
        """Читає нові рядки з файлу, повертає список нових подій."""
        if self._file is None:
            if not self.open():
                return []

        events = []
        self._file.seek(self._pos)
        for line in self._file:
            ev = parse_line(line, self._ref_date)
            if ev:
                events.append(ev)
        self._pos = self._file.tell()

        # Перевірити чи з'явився новий log файл
        latest = self._find_latest_log()
        if latest and latest != self._path:
            self._file.close()
            self.open(latest, seek_end=False)

        return events

    async def run_async(self, interval: float = 2.0):
        """Безкінечний асинхронний loop: drain + push до queue."""
        self._running = True
        while self._running:
            events = self.drain()
            if self.queue:
                for ev in events:
                    await self.queue.put(ev)
            await asyncio.sleep(interval)

    def stop(self):
        self._running = False
        if self._file:
            self._file.close()
