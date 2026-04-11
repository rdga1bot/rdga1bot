"""
live_stats.py — читач /tmp/rdga1bot_stats.json (real-time live stats).

Очікуваний формат (після розширення Stats::SaveToFile()):
  {"ts":1712345678,"kills":150,"deaths":0,"attacks":2182,
   "targeting_failures":18,"uptime_sec":1099}

Якщо файл не існує — повертає порожній dict (graceful degradation).
"""

import json
import os
import time
from typing import Optional

LIVE_STATS_PATH = "/tmp/rdga1bot_stats.json"
STALE_THRESHOLD_SEC = 15.0   # якщо файл не оновлювався > 15с → bot_frozen


class LiveStatsReader:
    """Читає /tmp/rdga1bot_stats.json кожні N секунд."""

    def __init__(self, path: str = LIVE_STATS_PATH):
        self.path = path
        self._last_data: dict = {}
        self._last_mtime: float = 0.0
        self._last_read_time: float = 0.0

    def poll(self) -> dict:
        """
        Зчитує файл та повертає dict з полями:
          available, stale, bot_frozen, ts, kills, deaths, attacks,
          targeting_failures, uptime_sec, age_sec
        """
        now = time.time()
        self._last_read_time = now

        result = {
            "available": False,
            "stale": False,
            "bot_frozen": False,
            "ts": None,
            "kills": 0,
            "deaths": 0,
            "attacks": 0,
            "targeting_failures": 0,
            "uptime_sec": 0,
            "age_sec": None,
        }

        if not os.path.exists(self.path):
            return result

        try:
            mtime = os.path.getmtime(self.path)
            age = now - mtime
            result["age_sec"] = round(age, 1)
            result["available"] = True

            if age > STALE_THRESHOLD_SEC:
                result["stale"] = True
                result["bot_frozen"] = True

            with open(self.path, "r", encoding="utf-8") as fh:
                data = json.load(fh)

            if isinstance(data, dict):
                result.update({k: data[k] for k in
                    ("ts", "kills", "deaths", "attacks",
                     "targeting_failures", "uptime_sec")
                    if k in data})
                self._last_data = data
                self._last_mtime = mtime

        except (OSError, json.JSONDecodeError, KeyError):
            pass

        return result

    @property
    def last_data(self) -> dict:
        return self._last_data

    def is_available(self) -> bool:
        """Перевіряє чи існує файл live stats."""
        return os.path.exists(self.path)
