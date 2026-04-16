#!/usr/bin/env python3
"""
qa_monitor.py — головний daemon QA моніторингу rdga1bot.

Використання:
  python qa/qa_monitor.py --log logs/session_*.log --duration 5h
  python qa/qa_monitor.py --log logs/session_*.log --duration 5h --with-mempalace
  python qa/qa_monitor.py --replay logs/stats_*.json
  python qa/qa_monitor.py --analyze-only logs/stats_2026-04-07.json
  python qa/qa_monitor.py --analyze-only logs/stats_2026-04-07.json --log logs/session_20260407_194538.log
"""

import argparse
import asyncio
import glob
import json
import logging
import os
import sys
import time
from datetime import datetime, timedelta
from pathlib import Path

# Додаємо qa/ до шляху пошуку модулів
_QA_DIR = os.path.dirname(os.path.abspath(__file__))
_ROOT_DIR = os.path.dirname(_QA_DIR)
sys.path.insert(0, _QA_DIR)
sys.path.insert(0, _ROOT_DIR)

from parsers.stats_reader import (
    load_file, load_all, compute_deltas,
    compute_kill_rate, summarize
)
from parsers.log_tailer import LogTailer, parse_file as parse_log_file
from parsers.live_stats import LiveStatsReader
from anomaly_engine import AdaptiveAnomalyEngine
from alert_manager import AlertManager
from dashboard_gen import DashboardGen
from screen_capture import ScreenCapture
from mempalace_bridge import MemPalaceBridge

# ---------------------------------------------------------------------------
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(levelname)-8s %(message)s",
    datefmt="%H:%M:%S",
)
logger = logging.getLogger("qa")

REPORTS_DIR = os.path.join(_QA_DIR, "reports")


# ---------------------------------------------------------------------------
# Утиліти
# ---------------------------------------------------------------------------

def _fmt_uptime(seconds: float) -> str:
    s = int(seconds)
    return f"{s//3600}h {(s%3600)//60}m {s%60}s"


def _find_most_recent_log(log_dir: str = None) -> str | None:
    """Знаходить найновіший session_*.log."""
    if log_dir is None:
        log_dir = os.path.join(_ROOT_DIR, "logs")
    candidates = glob.glob(os.path.join(log_dir, "session_*.log"))
    if not candidates:
        return None
    return max(candidates, key=os.path.getmtime)


# ---------------------------------------------------------------------------
# Analyze-only mode
# ---------------------------------------------------------------------------

def _extract_session_start(log_path: str) -> datetime | None:
    """Витягує datetime початку сесії з імені файлу session_YYYYMMDD_HHMMSS.log."""
    import re
    basename = os.path.basename(log_path)
    m = re.search(r'session_(\d{4})(\d{2})(\d{2})_(\d{2})(\d{2})(\d{2})', basename)
    if not m:
        return None
    try:
        return datetime(
            int(m.group(1)), int(m.group(2)), int(m.group(3)),
            int(m.group(4)), int(m.group(5)), int(m.group(6)),
        )
    except ValueError:
        return None


def _filter_records_by_session(records: list, session_start: datetime,
                                session_end: datetime | None = None) -> list:
    """
    Відфільтровує records за часовим вікном сесії.
    1. Залишає записи з timestamp_dt >= session_start - 60s
    2. Обрізає по session_end якщо відомий, або по першому uptime/kills reset
       (ознака старту нової сесії бота).
    """
    from datetime import timezone

    def make_aware(dt):
        if dt.tzinfo is None:
            return dt.replace(tzinfo=timezone.utc)
        return dt

    lo = make_aware(session_start - timedelta(seconds=60))
    hi = make_aware(session_end + timedelta(seconds=60)) if session_end else None

    # Крок 1 — фільтр по нижній межі
    candidates = []
    for r in records:
        ts = r.get("timestamp_dt")
        if ts is None:
            continue
        ts = make_aware(ts)
        if ts < lo:
            continue
        if hi and ts > hi:
            break
        candidates.append(r)

    if not candidates:
        return candidates

    # Крок 2 — обрізати по першому reset нової сесії бота.
    # Детектори:
    #   A) uptime впав на > 60с (явний restart)
    #   B) kills впали на > 5 (лічильник скинуто)
    #   C) приріст uptime << приріст wall-clock (бот перезапустився між записами)
    #      якщо elapsed_wall > 120с і elapsed_uptime < elapsed_wall * 0.35 → нова сесія
    result = [candidates[0]]
    for i in range(1, len(candidates)):
        prev = candidates[i - 1]
        curr = candidates[i]
        prev_uptime = prev.get("uptime_sec", 0)
        curr_uptime = curr.get("uptime_sec", 0)
        prev_kills  = prev.get("kills", 0)
        curr_kills  = curr.get("kills", 0)

        # A: uptime явно впав
        if curr_uptime < prev_uptime - 60:
            break
        # B: kills впали
        if curr_kills < prev_kills - 5:
            break
        # C: wall-clock gap >> uptime gap → restart між записами
        ts_prev = prev.get("timestamp_dt")
        ts_curr = curr.get("timestamp_dt")
        if ts_prev and ts_curr:
            from datetime import timezone
            def aw(dt):
                return dt if dt.tzinfo else dt.replace(tzinfo=timezone.utc)
            elapsed_wall   = (aw(ts_curr) - aw(ts_prev)).total_seconds()
            elapsed_uptime = curr_uptime - prev_uptime
            if elapsed_wall > 120 and elapsed_uptime < elapsed_wall * 0.35:
                break

        result.append(curr)

    return result


def run_analyze_only(stats_path: str, log_path: str | None = None) -> dict:
    """
    Повний аналіз одного stats JSON файлу (+ опціонально session log).
    Генерує звіт у qa/reports/.
    Якщо log_path вказано — фільтрує stats records за часовим вікном сесії.
    """
    logger.info(f"[QA] Завантажуємо: {stats_path}")

    all_records = load_file(stats_path)
    if not all_records:
        logger.error(f"[QA] Файл порожній або всі рядки corrupted: {stats_path}")
        return {}

    logger.info(f"[QA] Всього записів у файлі: {len(all_records)}")

    # Фільтрація за часовим вікном сесії якщо є log_path
    records = all_records
    if log_path:
        session_start = _extract_session_start(log_path)
        if session_start:
            # Визначаємо session_end — пізніше буде уточнено з подій логу
            # Спочатку фільтруємо по session_start (session_end невідомий)
            records = _filter_records_by_session(all_records, session_start)
            logger.info(f"[QA] Після фільтрації за сесією ({session_start.strftime('%H:%M:%S')}): "
                        f"{len(records)} записів (було {len(all_records)})")
            if not records:
                logger.warning("[QA] Записів у вікні сесії не знайдено — використовуємо всі")
                records = all_records

    records = compute_deltas(records)
    records = compute_kill_rate(records)
    summary = summarize(records)

    logger.info(f"[QA] Завантажено {len(records)} записів з {stats_path}")
    logger.info(f"[QA] Підсумок: kills={summary['total_kills']}, "
                f"deaths={summary['total_deaths']}, "
                f"duration={summary['duration_min']:.0f}хв, "
                f"kill_rate={summary['kill_rate_per_min']:.2f}/хв")

    engine = AdaptiveAnomalyEngine()

    # Аналіз stats records
    anomalies = engine.analyze_stats_records(records)

    # Якщо є лог файл — парсимо і додаємо події
    log_events = []
    if log_path and os.path.exists(log_path):
        logger.info(f"[QA] Парсимо лог: {log_path}")
        log_events = parse_log_file(log_path)
        logger.info(f"[QA] Знайдено {len(log_events)} подій у логу")
        for ev in log_events:
            ev["_ingest_time"] = ev.get("timestamp") and ev["timestamp"].timestamp() or time.time()
            engine.ingest_event(ev)
        # Повторний check після ingestion логів
        extra = engine.rules.check()
        for a in extra:
            a["ts"] = "log"
            a["timestamp"] = time.time()
        anomalies.extend(extra)

    # Підрахунок за severity
    by_sev = {}
    for a in anomalies:
        sev = a.get("severity", "INFO")
        by_sev[sev] = by_sev.get(sev, 0) + 1

    sev_str = ", ".join(f"{v} {k}" for k, v in sorted(by_sev.items()))
    total_anomalies = len(anomalies)
    logger.info(f"[QA] Знайдено аномалій: {sev_str or 'немає'}")

    # Генерація dashboard
    dash = DashboardGen()

    # Додаємо kill rate точки для графіку
    for rec in records:
        ts_dt = rec.get("timestamp_dt")
        if ts_dt:
            ts_str = ts_dt.strftime("%H:%M:%S")
            rate = rec.get("kill_rate_per_min", 0.0)
            dash.push_kill_rate(ts_str, rate)

    # Score по часу (приблизно)
    if anomalies:
        for a in anomalies:
            ts = a.get("ts", "??:??:??")
            score = float(a.get("score", 0))
            dash.push_score(ts, score)

    live_stats = {"available": False}
    dash.render(
        session_name=os.path.basename(stats_path),
        total_score=max((a.get("score", 0) for a in anomalies), default=0),
        uptime_str=_fmt_uptime(summary.get("duration_min", 0) * 60),
        anomalies=anomalies,
        live_stats=live_stats,
        frames=[],
    )
    logger.info(f"[QA] Dashboard: {dash.output_path}")

    # Генерація HTML звіту
    os.makedirs(REPORTS_DIR, exist_ok=True)
    report_name = f"session_{datetime.now().strftime('%Y%m%d_%H%M%S')}.html"
    report_path = os.path.join(REPORTS_DIR, report_name)
    _write_report(report_path, stats_path, summary, anomalies, log_events)
    logger.info(f"[QA] Звіт: {report_path}")

    return {
        "records": len(records),
        "summary": summary,
        "anomalies": anomalies,
        "log_events": len(log_events),
        "report": report_path,
        "dashboard": dash.output_path,
    }


def _write_report(path: str, source: str, summary: dict,
                  anomalies: list, log_events: list):
    """Генерує HTML звіт сесії."""
    ts_now = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    anom_rows = ""
    for a in anomalies[:50]:
        sev = a.get("severity", "INFO")
        color = {"INFO": "#4caf50", "WARNING": "#ff9800",
                 "ERROR": "#f44336", "CRITICAL": "#9c27b0"}.get(sev, "#9e9e9e")
        anom_rows += (
            f"<tr>"
            f"<td>{a.get('ts','')}</td>"
            f"<td style='color:{color}'><b>{sev}</b></td>"
            f"<td>{a.get('name','')}</td>"
            f"<td>{a.get('score',0)}</td>"
            f"<td>{a.get('context','')}</td>"
            f"</tr>"
        )
    if not anom_rows:
        anom_rows = "<tr><td colspan='5' style='color:#9e9e9e'>Аномалій не виявлено</td></tr>"

    html = f"""<!DOCTYPE html>
<html lang="uk">
<head>
  <meta charset="UTF-8">
  <title>rdga1bot QA Session Report</title>
  <style>
    body {{ background:#1a1a2e; color:#e0e0e0; font-family:monospace; padding:20px; }}
    h1 {{ color:#90caf9; }} h2 {{ color:#64b5f6; margin-top:16px; }}
    table {{ border-collapse:collapse; width:100%; font-size:0.85em; }}
    th {{ background:#0f3460; padding:6px 10px; text-align:left; }}
    td {{ padding:5px 10px; border-bottom:1px solid #222; }}
    .stat {{ display:inline-block; background:#16213e; border-radius:6px; padding:8px 14px; margin:4px; }}
    .val {{ font-size:1.4em; font-weight:bold; color:#90caf9; }}
  </style>
</head>
<body>
<h1>rdga1bot QA Session Report</h1>
<p style="color:#9e9e9e">Джерело: {source}<br>Згенеровано: {ts_now}</p>

<h2>Статистика сесії</h2>
<div>
  <div class="stat"><div class="val">{summary.get('total_kills',0)}</div><div>Kills</div></div>
  <div class="stat"><div class="val">{summary.get('total_deaths',0)}</div><div>Deaths</div></div>
  <div class="stat"><div class="val">{summary.get('kill_rate_per_min',0):.2f}</div><div>Kill/min</div></div>
  <div class="stat"><div class="val">{summary.get('duration_min',0):.0f}</div><div>Хвилин</div></div>
  <div class="stat"><div class="val">{summary.get('total_targeting_failures',0)}</div><div>TF</div></div>
  <div class="stat"><div class="val">{summary.get('tf_per_kill',0):.2f}</div><div>TF/Kill</div></div>
  <div class="stat"><div class="val">{summary.get('record_count',0)}</div><div>Записів</div></div>
</div>

<h2>Аномалії ({len(anomalies)})</h2>
<table>
<thead><tr><th>Час</th><th>Severity</th><th>Детектор</th><th>Score</th><th>Контекст</th></tr></thead>
<tbody>{anom_rows}</tbody>
</table>

<h2>Лог подій ({len(log_events)} розпізнано)</h2>
<p style="color:#9e9e9e; font-size:0.8em">Типи подій: {
    ", ".join(f"{k}:{v}" for k,v in
              __import__('collections').Counter(e.get('type','?') for e in log_events).most_common(10))
    if log_events else "лог не надано"
}</p>

</body>
</html>"""

    with open(path, "w", encoding="utf-8") as fh:
        fh.write(html)


# ---------------------------------------------------------------------------
# Replay mode
# ---------------------------------------------------------------------------

def run_replay(stats_paths: list[str], log_path: str | None = None) -> dict:
    """
    Завантажує архівні JSON + навчає IsolationForest,
    потім генерує зведений звіт.

    Кожен файл = окрема сесія. Аналіз і feature extraction — per-session,
    щоб уникнути false positives від reset kills=0 між сесіями.
    """
    engine = AdaptiveAnomalyEngine()
    all_records = []
    all_anomalies = []
    total_summary = {"total_kills": 0, "total_deaths": 0, "duration_min": 0.0}

    for p in sorted(stats_paths):
        recs = load_file(p)
        if not recs:
            continue
        recs = compute_deltas(recs)
        recs = compute_kill_rate(recs)
        s = summarize(recs)
        total_summary["total_kills"]  += s.get("total_kills", 0)
        total_summary["total_deaths"] += s.get("total_deaths", 0)
        total_summary["duration_min"] += s.get("duration_min", 0.0)

        # Аномалії per-session (analyze_stats_records сам виконує ingest)
        session_anoms = engine.analyze_stats_records(recs)

        # Feature vector після аналізу сесії (для IsolationForest)
        if len(recs) >= 3:
            features = engine.extract_features()
            engine.feature_history.append(features)
        all_anomalies.extend(session_anoms)
        all_records.extend(recs)

    if not all_records:
        logger.error("[QA] Не знайдено жодного валідного запису")
        return {}

    summary = total_summary
    logger.info(f"[QA] Replay: завантажено {len(all_records)} записів з {len(stats_paths)} файлів")
    logger.info(f"[QA] Підсумок: kills={summary['total_kills']}, "
                f"duration={summary['duration_min']:.0f}хв")

    # Тренуємо IsolationForest якщо накопичили достатньо feature vectors
    if len(engine.feature_history) >= 10:
        engine.retrain()
        if engine.is_trained:
            logger.info(f"[QA] IsolationForest навчено на {len(engine.feature_history)} feature vectors"
                        f" → qa/models/")

    anomalies = all_anomalies
    by_sev = {}
    for a in anomalies:
        sev = a.get("severity", "INFO")
        by_sev[sev] = by_sev.get(sev, 0) + 1
    sev_str = ", ".join(f"{v} {k}" for k, v in sorted(by_sev.items()))
    logger.info(f"[QA] Знайдено аномалій: {sev_str or 'немає'}")

    dash = DashboardGen()
    for rec in all_records[-60:]:
        ts_dt = rec.get("timestamp_dt")
        if ts_dt:
            dash.push_kill_rate(ts_dt.strftime("%H:%M:%S"), rec.get("kill_rate_per_min", 0))

    dash.render(
        session_name=f"Replay {len(stats_paths)} файлів",
        total_score=max((a.get("score", 0) for a in anomalies), default=0),
        uptime_str=_fmt_uptime(summary.get("duration_min", 0) * 60),
        anomalies=anomalies,
        live_stats={"available": False},
        frames=[],
    )
    logger.info(f"[QA] Dashboard: {dash.output_path}")

    os.makedirs(REPORTS_DIR, exist_ok=True)
    report_name = f"replay_{datetime.now().strftime('%Y%m%d_%H%M%S')}.html"
    report_path = os.path.join(REPORTS_DIR, report_name)
    _write_report(report_path, f"Replay {len(stats_paths)} files", summary, anomalies, [])
    logger.info(f"[QA] Звіт: {report_path}")

    return {"records": len(all_records), "anomalies": anomalies, "report": report_path,
            "model_trained": engine.is_trained,
            "feature_vectors": len(engine.feature_history)}


# ---------------------------------------------------------------------------
# Daemon mode (asyncio)
# ---------------------------------------------------------------------------

class QADaemon:
    """Головний асинхронний daemon."""

    POLL_LIVE_EVERY   = 5    # секунд
    DRAIN_LOG_EVERY   = 10
    SNAP_EVERY        = 30
    RENDER_EVERY      = 60
    RETRAIN_EVERY     = 600
    CHECK_EVERY       = 5

    def __init__(self, args, bridge: "MemPalaceBridge | None" = None):
        self.args   = args
        self.bridge = bridge
        self.engine    = AdaptiveAnomalyEngine()
        self.alerts    = AlertManager()
        self.dash      = DashboardGen()
        self.screen    = ScreenCapture()
        self.live      = LiveStatsReader()
        self.tailer    = LogTailer(log_dir=os.path.join(_ROOT_DIR, "logs"))
        self.event_queue: asyncio.Queue = asyncio.Queue()

        self._start_time = time.time()
        self._kills_10min_start = (0, time.time())
        self._score_tracker: list = []

    async def run(self):
        """Запускає всі асинхронні задачі."""
        log_path = getattr(self.args, "log", None)
        if log_path and not os.path.exists(log_path):
            # Файл не знайдено — шукаємо найновіший session_*.log поруч
            alt = _find_most_recent_log(os.path.dirname(log_path) or None)
            if alt:
                logger.warning(f"[QA] '{log_path}' не знайдено → використовуємо {os.path.basename(alt)}")
                log_path = alt
            else:
                raise FileNotFoundError(
                    f"'{log_path}' не знайдено і жодного session_*.log в logs/ немає.\n"
                    f"Використай: python qa/qa_monitor.py --log logs/session_<дата>.log"
                )
        if log_path:
            self.tailer.open(log_path, seek_end=False)
        else:
            self.tailer.open()

        tasks = [
            asyncio.create_task(self._live_poll_loop()),
            asyncio.create_task(self._log_drain_loop()),
            asyncio.create_task(self._snap_loop()),
            asyncio.create_task(self._render_loop()),
            asyncio.create_task(self._check_loop()),
        ]

        duration = getattr(self.args, "duration_sec", None)
        if duration:
            await asyncio.sleep(duration)
            for t in tasks:
                t.cancel()
        else:
            try:
                await asyncio.gather(*tasks)
            except asyncio.CancelledError:
                pass

    async def _live_poll_loop(self):
        while True:
            snap = self.live.poll()
            self.engine.ingest_stats(snap)
            if snap.get("available"):
                kills = snap.get("kills", 0)
                rate = self._calc_recent_kill_rate(kills)
                ts = datetime.now().strftime("%H:%M:%S")
                self.dash.push_kill_rate(ts, rate)
            await asyncio.sleep(self.POLL_LIVE_EVERY)

    async def _log_drain_loop(self):
        while True:
            events = self.tailer.drain()
            for ev in events:
                ev["_ingest_time"] = time.time()
                self.engine.ingest_event(ev)
                branch = ev.get("obj") or ev.get("branch")
                if branch:
                    self.dash.push_branch(branch)
            await asyncio.sleep(self.DRAIN_LOG_EVERY)

    async def _snap_loop(self):
        while True:
            await asyncio.sleep(self.SNAP_EVERY)
            self.screen.snap()

    async def _render_loop(self):
        while True:
            await asyncio.sleep(self.RENDER_EVERY)
            frames = self.screen.get_recent_frames(6)
            live = self.live.poll()
            self.dash.render(
                session_name=datetime.now().strftime("%Y-%m-%d"),
                total_score=self._current_score(),
                uptime_str=_fmt_uptime(time.time() - self._start_time),
                anomalies=self.engine.recent_anomalies,
                live_stats=live,
                frames=frames,
            )

    async def _check_loop(self):
        while True:
            await asyncio.sleep(self.CHECK_EVERY)
            result = self.engine.tick()
            total_score = result["total_score"]
            ts = result["ts"]

            self.dash.push_score(ts, total_score)
            self._score_tracker.append(total_score)
            if len(self._score_tracker) > 200:
                self._score_tracker.pop(0)

            anomalies = result["anomalies"]
            if anomalies:
                self.alerts.alert_many(anomalies, total_score)
                for a in anomalies:
                    logger.warning(f"[QA] {a['severity']} {a['name']}: {a.get('context','')}")
                    if self.bridge:
                        self.bridge.log_anomaly(a)

    def _current_score(self) -> float:
        return self._score_tracker[-1] if self._score_tracker else 0.0

    def _calc_recent_kill_rate(self, kills_now: int) -> float:
        """Kill rate за останні 10 хвилин."""
        now = time.time()
        start_kills, start_ts = self._kills_10min_start
        elapsed_min = (now - start_ts) / 60
        if elapsed_min > 10:
            self._kills_10min_start = (kills_now, now)
            return 0.0
        if elapsed_min < 0.1:
            return 0.0
        return max(kills_now - start_kills, 0) / elapsed_min


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def parse_duration(s: str) -> int:
    """Парсить '5h', '30m', '3600' → секунди."""
    s = s.strip().lower()
    if s.endswith("h"):
        return int(s[:-1]) * 3600
    elif s.endswith("m"):
        return int(s[:-1]) * 60
    else:
        return int(s)


def main():
    parser = argparse.ArgumentParser(
        description="rdga1bot QA Monitor",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""Приклади:
  python qa/qa_monitor.py --analyze-only logs/stats_2026-04-07.json
  python qa/qa_monitor.py --analyze-only logs/stats_2026-04-07.json --log logs/session_20260407_194538.log
  python qa/qa_monitor.py --replay logs/stats_2026-04-*.json
  python qa/qa_monitor.py --log logs/session_20260407_194538.log --duration 5h
""",
    )

    parser.add_argument(
        "--analyze-only",
        metavar="STATS_JSON",
        help="Аналіз одного stats_*.json файлу без daemon",
    )
    parser.add_argument(
        "--log",
        metavar="SESSION_LOG",
        help="Шлях до session_*.log (для daemon або --analyze-only)",
    )
    parser.add_argument(
        "--replay",
        nargs="+",
        metavar="STATS_JSON",
        help="Завантажити архівні JSON + навчити IsolationForest",
    )
    parser.add_argument(
        "--duration",
        metavar="TIME",
        help="Тривалість daemon режиму: 5h, 30m, 3600",
    )
    parser.add_argument(
        "--no-screen",
        action="store_true",
        help="Вимкнути scrot захоплення",
    )
    parser.add_argument(
        "--with-mempalace",
        action="store_true",
        help="Увімкнути MemPalace інтеграцію (anomalies.jsonl + семантичний baseline)",
    )
    parser.add_argument(
        "-v", "--verbose",
        action="store_true",
        help="DEBUG рівень логування",
    )

    args = parser.parse_args()

    if args.verbose:
        logging.getLogger().setLevel(logging.DEBUG)

    # Змінюємо CWD на корінь проекту щоб відносні шляхи працювали
    os.chdir(_ROOT_DIR)

    # MemPalace bridge (graceful — не падає якщо вимкнено)
    bridge = MemPalaceBridge(_ROOT_DIR) if getattr(args, "with_mempalace", False) else None

    # ---- Режим 1: analyze-only ----
    if args.analyze_only:
        # Підтримка wildcard та конкретного файлу
        stats_path = args.analyze_only
        if not os.path.exists(stats_path):
            # Спробуємо додати logs/ prefix або .json суфікс
            candidates = [
                stats_path,
                os.path.join("logs", stats_path),
                os.path.join("logs", f"stats_{stats_path}.json"),
            ]
            for c in candidates:
                if os.path.exists(c):
                    stats_path = c
                    break
            else:
                # glob fallback
                matches = glob.glob(stats_path) or glob.glob(f"logs/{stats_path}")
                if matches:
                    stats_path = sorted(matches)[-1]
                else:
                    print(f"[QA] Файл не знайдено: {args.analyze_only}", file=sys.stderr)
                    sys.exit(1)

        result = run_analyze_only(stats_path, log_path=args.log)
        if result:
            print(f"\n[QA] Готово.")
            print(f"[QA] Records:   {result.get('records', 0)}")
            print(f"[QA] Anomalies: {len(result.get('anomalies', []))}")
            print(f"[QA] Dashboard: {result.get('dashboard', '')}")
            print(f"[QA] Report:    {result.get('report', '')}")
        return

    # ---- Режим 2: replay ----
    if args.replay:
        paths = []
        for pattern in args.replay:
            expanded = glob.glob(pattern)
            if expanded:
                paths.extend(expanded)
            elif os.path.exists(pattern):
                paths.append(pattern)

        if not paths:
            print("[QA] Не знайдено файлів для replay", file=sys.stderr)
            sys.exit(1)

        paths = sorted(set(paths))
        logger.info(f"[QA] Replay: {len(paths)} файлів")
        result = run_replay(paths, log_path=args.log)
        if result:
            print(f"\n[QA] Готово.")
            print(f"[QA] Records:   {result.get('records', 0)}")
            print(f"[QA] Anomalies: {len(result.get('anomalies', []))}")
            print(f"[QA] Report:    {result.get('report', '')}")
        return

    # ---- Режим 3: daemon ----
    logger.info("[QA] Запуск daemon режиму...")

    duration_sec = None
    if args.duration:
        duration_sec = parse_duration(args.duration)
        logger.info(f"[QA] Тривалість: {duration_sec}с")
    args.duration_sec = duration_sec

    daemon = QADaemon(args, bridge=bridge)
    try:
        asyncio.run(daemon.run())
    except KeyboardInterrupt:
        logger.info("[QA] Зупинено (Ctrl+C)")
    finally:
        if bridge:
            bridge.trigger_mining()


if __name__ == "__main__":
    main()
