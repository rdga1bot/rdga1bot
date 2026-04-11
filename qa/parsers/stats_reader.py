"""
stats_reader.py — читач *.json файлів сесій rdga1bot.

Реальна структура рядків (одна JSON-об'єкт на рядок):
  {"timestamp":"2026-04-07T00:06:04","uptime_sec":1099,"kills":150,"deaths":0,
   "attacks":2182,"hp_potions":0,"mp_potions":0,"targeting_failures":18}
"""

import json
import os
from datetime import datetime, timezone
from pathlib import Path
from typing import Iterator, List, Optional


KNOWN_FIELDS = {
    "timestamp", "uptime_sec", "kills", "deaths", "attacks",
    "hp_potions", "mp_potions", "targeting_failures",
}

# Sanity limits — відфільтровує corrupted рядки типу kills=1479372432
_SANE_KILLS_MAX   = 1_000_000
_SANE_UPTIME_MAX  = 7 * 24 * 3600   # тиждень


def _parse_ts(ts_str: str) -> Optional[datetime]:
    """Парсить ISO-рядок в datetime (UTC-aware або naive)."""
    try:
        # Python 3.11+ підтримує fromisoformat з timezone
        dt = datetime.fromisoformat(ts_str)
        if dt.tzinfo is None:
            dt = dt.replace(tzinfo=timezone.utc)
        return dt
    except (ValueError, TypeError):
        return None


def _is_sane(rec: dict) -> bool:
    """Відкидає явно corrupted записи (переповнення int32 тощо)."""
    kills = rec.get("kills", 0)
    uptime = rec.get("uptime_sec", 0)
    return (
        isinstance(kills, (int, float)) and 0 <= kills <= _SANE_KILLS_MAX
        and isinstance(uptime, (int, float)) and 0 <= uptime <= _SANE_UPTIME_MAX
    )


def _normalize(rec: dict, filename_date: str = "") -> dict:
    """
    Нормалізує один JSON-рядок → стандартний dict зі всіма відомими полями.
    Невідомі поля зберігаються як є (розширення майбутніх версій).
    """
    out = {
        "timestamp": None,
        "uptime_sec": 0,
        "kills": 0,
        "deaths": 0,
        "attacks": 0,
        "hp_potions": 0,
        "mp_potions": 0,
        "targeting_failures": 0,
        "_source_date": filename_date,
    }
    out.update(rec)

    ts_raw = out.get("timestamp")
    if isinstance(ts_raw, str):
        out["timestamp_dt"] = _parse_ts(ts_raw)
    else:
        out["timestamp_dt"] = None

    return out


def iter_records(path: str | Path) -> Iterator[dict]:
    """
    Ітерує всі валідні JSON-рядки з файлу stats_*.json.
    Пропускає corrupted / порожні рядки.
    """
    path = Path(path)
    filename_date = path.stem.replace("stats_", "")

    with open(path, "r", encoding="utf-8") as fh:
        for lineno, line in enumerate(fh, 1):
            line = line.strip()
            if not line:
                continue
            try:
                rec = json.loads(line)
            except json.JSONDecodeError:
                continue
            if not isinstance(rec, dict):
                continue
            if not _is_sane(rec):
                continue
            yield _normalize(rec, filename_date)


def load_file(path: str | Path) -> List[dict]:
    """Завантажує весь файл в список записів."""
    return list(iter_records(path))


def load_all(log_dir: str | Path = "logs") -> List[dict]:
    """Завантажує всі stats_*.json файли з каталогу, сортує за timestamp."""
    log_dir = Path(log_dir)
    all_records = []
    for f in sorted(log_dir.glob("stats_*.json")):
        all_records.extend(iter_records(f))
    all_records.sort(key=lambda r: r.get("timestamp", "") or "")
    return all_records


def compute_kill_rate(records: List[dict], window_minutes: int = 10) -> List[dict]:
    """
    Додає поле 'kill_rate_per_min' для кожного запису
    на основі ковзного вікна (window_minutes).
    """
    if not records:
        return records

    result = []
    for i, rec in enumerate(records):
        ts_i = rec.get("timestamp_dt")
        if ts_i is None:
            rec["kill_rate_per_min"] = 0.0
            result.append(rec)
            continue

        # Зібрати записи в межах вікна
        window_kills = 0
        window_start = None
        window_end = ts_i

        for j in range(i, -1, -1):
            ts_j = records[j].get("timestamp_dt")
            if ts_j is None:
                continue
            delta = (window_end - ts_j).total_seconds() / 60
            if delta > window_minutes:
                break
            if window_start is None or ts_j < window_start:
                window_start = ts_j
            window_kills = rec["kills"] - records[j]["kills"]
            if window_kills < 0:
                window_kills = rec["kills"]

        if window_start is not None and window_start != window_end:
            mins = (window_end - window_start).total_seconds() / 60
            rec["kill_rate_per_min"] = window_kills / max(mins, 0.1)
        else:
            rec["kill_rate_per_min"] = 0.0

        result.append(rec)
    return result


def compute_deltas(records: List[dict]) -> List[dict]:
    """
    Для кожного запису обчислює дельту від попереднього:
    d_kills, d_attacks, d_targeting_failures, d_deaths, elapsed_sec
    """
    if not records:
        return records

    prev = records[0]
    records[0]["d_kills"] = 0
    records[0]["d_attacks"] = 0
    records[0]["d_targeting_failures"] = 0
    records[0]["d_deaths"] = 0
    records[0]["elapsed_sec"] = 0

    for i in range(1, len(records)):
        rec = records[i]
        pp = records[i - 1]
        for field in ("kills", "attacks", "targeting_failures", "deaths"):
            delta = rec.get(field, 0) - pp.get(field, 0)
            rec[f"d_{field}"] = max(delta, 0)  # захист від reset

        ts_curr = rec.get("timestamp_dt")
        ts_prev = pp.get("timestamp_dt")
        if ts_curr and ts_prev:
            rec["elapsed_sec"] = max((ts_curr - ts_prev).total_seconds(), 0)
        else:
            rec["elapsed_sec"] = 0

    return records


def summarize(records: List[dict]) -> dict:
    """Генерує підсумок сесії з усіх записів."""
    if not records:
        return {}

    total_kills = max(r.get("kills", 0) for r in records)
    total_deaths = max(r.get("deaths", 0) for r in records)
    total_attacks = max(r.get("attacks", 0) for r in records)
    total_tf = max(r.get("targeting_failures", 0) for r in records)
    total_hp_pots = max(r.get("hp_potions", 0) for r in records)
    total_mp_pots = max(r.get("mp_potions", 0) for r in records)

    ts_first = records[0].get("timestamp_dt")
    ts_last  = records[-1].get("timestamp_dt")
    duration_min = 0.0
    if ts_first and ts_last:
        duration_min = max((ts_last - ts_first).total_seconds() / 60, 0.1)

    return {
        "total_kills": total_kills,
        "total_deaths": total_deaths,
        "total_attacks": total_attacks,
        "total_targeting_failures": total_tf,
        "total_hp_potions": total_hp_pots,
        "total_mp_potions": total_mp_pots,
        "duration_min": round(duration_min, 1),
        "kill_rate_per_min": round(total_kills / duration_min, 2),
        "tf_per_kill": round(total_tf / max(total_kills, 1), 2),
        "record_count": len(records),
    }
