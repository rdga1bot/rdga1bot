"""
anomaly_engine.py — rule-based + adaptive IsolationForest детектори аномалій.

Кожен rule-based детектор повертає:
  {"name": str, "score": int, "severity": str, "context": str, "ts": datetime|None}

Загальний score тіку = sum(rule_scores) + adaptive_score * 20
  > 30  → WARNING в dashboard
  > 50  → notify-send алерт + запис в anomaly log
  > 80  → CRITICAL алерт + terminal bell
"""

import os
import glob
import time
import math
import logging
from collections import deque
from datetime import datetime, timezone
from typing import Optional, List

import numpy as np
import joblib

try:
    from sklearn.ensemble import IsolationForest
    _SKLEARN_OK = True
except ImportError:
    _SKLEARN_OK = False

logger = logging.getLogger("qa.anomaly")

# ---------------------------------------------------------------------------
# Константи
# ---------------------------------------------------------------------------
SCORE_WARNING  = 30
SCORE_ALERT    = 50
SCORE_CRITICAL = 80

WARMUP_MINUTES   = 30
RETRAIN_INTERVAL = 600   # 10 хвилин

MODEL_DIR = os.path.join(os.path.dirname(__file__), "models")


# ---------------------------------------------------------------------------
# Rule-based детектори
# ---------------------------------------------------------------------------

class RuleEngine:
    """
    Зберігає state для rule-based детекторів.
    Виклик check(events, stats_snapshot) → список anomaly dicts.
    """

    def __init__(self):
        # Sliding windows (deque of (timestamp_float, event_or_value))
        self._dead_events: deque    = deque()   # 10-хв вікно смертей
        self._blacklist_events: deque = deque() # 5-хв вікно blacklist
        self._tgt_macro_events: deque = deque() # 2с вікно
        self._branch_history: deque = deque()   # (ts, branch)  — 1-год вікно
        self._target_hp_history: deque = deque()# (ts, hp_pct)

        self._targeting_failures_prev: Optional[int] = None
        self._tf_window: deque = deque()        # 1-хв вікно tf дельт
        self._kills_window: deque = deque()     # 10-хв вікно kills snapshot
        self._baseline_kill_rate: float = 0.0
        self._baseline_samples: int = 0
        self._bt_avg_us_history: deque = deque()  # останні 20 значень BT avg

        self._live_stats_last_ts: Optional[float] = None
        self._last_ingest_ts: float = time.time()  # час останнього ingested запису

    # ---- helpers -----------------------------------------------------------

    def _prune(self, q: deque, max_age_sec: float, ref_time: float = None):
        """Видаляє старі записи з deque.
        ref_time: якщо None — використовує time.time() (live режим);
                  в analyze режимі передається ts останнього запису.
        """
        now = ref_time if ref_time is not None else time.time()
        while q and (now - q[0][0]) > max_age_sec:
            q.popleft()

    # ---- ingest ------------------------------------------------------------

    def ingest_event(self, ev: dict):
        """Додає подію до відповідних вікон."""
        now = ev.get("_ingest_time") or time.time()
        etype = ev.get("type")

        if etype == "dead":
            self._dead_events.append((now, ev))

        elif etype == "blacklist":
            self._blacklist_events.append((now, ev))

        elif etype == "bt_tick":
            branch = ev.get("branch", "")
            avg_us = ev.get("avg_us", 0)
            self._branch_history.append((now, branch))
            if avg_us:
                self._bt_avg_us_history.append(avg_us)
                if len(self._bt_avg_us_history) > 200:
                    self._bt_avg_us_history.popleft()

        elif etype == "heartbeat":
            branch = ev.get("obj", "")
            self._branch_history.append((now, branch))
            hp = ev.get("hp")
            if branch == "Attack" and hp is not None:
                self._target_hp_history.append((now, hp))

        elif etype == "targeting_attempt":
            pass  # handled by stats

    def ingest_stats(self, snap: dict, ts: float = None):
        """Обробляє snapshot з StatsReader або LiveStats.
        ts — реальний час запису (float unix); якщо None — використовує time.time().
        """
        now = ts if ts is not None else time.time()
        tf = snap.get("targeting_failures")
        kills = snap.get("kills")
        uptime = snap.get("uptime_sec")

        if tf is not None and self._targeting_failures_prev is not None:
            delta = tf - self._targeting_failures_prev
            if delta > 0:
                self._tf_window.append((now, delta))
        if tf is not None:
            self._targeting_failures_prev = tf

        if kills is not None and uptime:
            self._kills_window.append((now, kills, uptime))
            # Оновлюємо baseline kill rate (середнє per session)
            if uptime > 120:  # мінімум 2 хв даних
                rate = kills / (uptime / 60)
                self._baseline_kill_rate = (
                    self._baseline_kill_rate * self._baseline_samples + rate
                ) / (self._baseline_samples + 1)
                self._baseline_samples += 1

        live_ts = snap.get("ts")
        if live_ts:
            self._live_stats_last_ts = float(live_ts)

    # ---- checks ------------------------------------------------------------

    def check(self) -> List[dict]:
        """Запускає всі rule-based детектори, повертає список аномалій."""
        anomalies = []
        now = time.time()

        anomalies.extend(self._check_death_loop(now))
        anomalies.extend(self._check_blacklist_flood(now))
        anomalies.extend(self._check_stuck_in_target(now))
        anomalies.extend(self._check_hp_stable(now))
        anomalies.extend(self._check_bot_frozen(now))
        anomalies.extend(self._check_tf_spike(now))
        anomalies.extend(self._check_kill_rate_drop(now))
        anomalies.extend(self._check_slow_bt_tick())

        return anomalies

    def _check_death_loop(self, now: float) -> List[dict]:
        """[DEAD] > 3 разів за 10 хвилин."""
        self._prune(self._dead_events, 600)
        count = len(self._dead_events)
        if count > 3:
            return [{"name": "death_loop",
                     "score": 50,
                     "severity": "CRITICAL",
                     "context": f"{count} deaths in last 10min"}]
        return []

    def _check_blacklist_flood(self, now: float) -> List[dict]:
        """[BLACKLIST] > 10 разів за 5 хвилин."""
        self._prune(self._blacklist_events, 300)
        count = len(self._blacklist_events)
        if count > 10:
            return [{"name": "blacklist_flood",
                     "score": 25,
                     "severity": "WARNING",
                     "context": f"{count} blacklists in last 5min"}]
        return []

    def _check_stuck_in_target(self, now: float) -> List[dict]:
        """BT OBJ=Target > 60с без переходу в Attack."""
        self._prune(self._branch_history, 3600)
        if len(self._branch_history) < 5:
            return []

        # Знайти останній перехід до Attack
        last_attack_ts = None
        for ts, branch in reversed(list(self._branch_history)):
            if branch in ("Attack", "Loot", "Buff", "Dead", "Rest"):
                last_attack_ts = ts
                break

        # Перевірити чи зараз довго в Target
        target_streak_start = None
        for ts, branch in reversed(list(self._branch_history)):
            if branch == "Target":
                target_streak_start = ts
            else:
                break

        if target_streak_start is None:
            return []

        target_duration = now - target_streak_start
        if target_duration > 60:
            return [{"name": "stuck_in_target",
                     "score": 40,
                     "severity": "WARNING",
                     "context": f"Target branch for {target_duration:.0f}s without Attack"}]
        return []

    def _check_hp_stable(self, now: float) -> List[dict]:
        """HP моба не змінюється > 30с під час Attack."""
        self._prune(self._target_hp_history, 120)
        if len(self._target_hp_history) < 3:
            return []

        window = list(self._target_hp_history)
        if len(window) < 3:
            return []

        oldest_ts, oldest_hp = window[0]
        newest_ts, newest_hp = window[-1]
        duration = newest_ts - oldest_ts

        if duration > 30 and abs(oldest_hp - newest_hp) < 3:
            return [{"name": "hp_stable_during_attack",
                     "score": 30,
                     "severity": "ERROR",
                     "context": f"Target HP ~{newest_hp}% unchanged for {duration:.0f}s"}]
        return []

    def _check_bot_frozen(self, now: float) -> List[dict]:
        """/tmp stats не оновлювався > 15с."""
        if self._live_stats_last_ts is None:
            return []
        age = now - self._live_stats_last_ts
        if age > 15:
            return [{"name": "bot_frozen",
                     "score": 80,
                     "severity": "CRITICAL",
                     "context": f"/tmp/rdga1bot_stats.json stale {age:.0f}s"}]
        return []

    def _check_tf_spike(self, now: float) -> List[dict]:
        """targeting_failures зростає > 5 за хвилину."""
        self._prune(self._tf_window, 60)
        total_tf = sum(d for _, d in self._tf_window)
        if total_tf > 5:
            return [{"name": "targeting_failure_spike",
                     "score": 35,
                     "severity": "ERROR",
                     "context": f"+{total_tf} targeting failures in last 1min"}]
        return []

    def _check_kill_rate_drop(self, now: float) -> List[dict]:
        """kills/хв < 50% від baseline за останні 10хв."""
        self._prune_kills(600)
        if self._baseline_kill_rate < 0.5 or len(self._kills_window) < 2:
            return []

        recent = list(self._kills_window)
        first_ts, first_kills, _ = recent[0]
        last_ts, last_kills, _ = recent[-1]
        elapsed_min = (last_ts - first_ts) / 60
        if elapsed_min < 1:
            return []

        recent_rate = (last_kills - first_kills) / elapsed_min
        if recent_rate < 0:
            return []

        if recent_rate < self._baseline_kill_rate * 0.5:
            return [{"name": "kill_rate_drop",
                     "score": 30,
                     "severity": "WARNING",
                     "context": f"Kill rate {recent_rate:.1f}/min vs baseline {self._baseline_kill_rate:.1f}/min"}]
        return []

    def _prune_kills(self, max_age: float):
        """Prune kills window."""
        now = time.time()
        while self._kills_window and (now - self._kills_window[0][0]) > max_age:
            self._kills_window.popleft()

    def _check_slow_bt_tick(self) -> List[dict]:
        """BT avg tick > 50µs."""
        if not self._bt_avg_us_history:
            return []
        recent = list(self._bt_avg_us_history)[-20:]
        avg = sum(recent) / len(recent)
        if avg > 50:
            return [{"name": "slow_bt_tick",
                     "score": 10,
                     "severity": "INFO",
                     "context": f"BT avg tick {avg:.0f}µs > 50µs threshold"}]
        return []

    # ---- stats-only check --------------------------------------------------

    def check_no_kills_window(self, kills_now: int, window_start_kills: int,
                               window_min: float) -> List[dict]:
        """0 kills за 10 хвилин."""
        if window_min >= 10 and kills_now <= window_start_kills:
            return [{"name": "no_kills_10min",
                     "score": 45,
                     "severity": "ERROR",
                     "context": f"0 kills in {window_min:.0f} min window"}]
        return []


# ---------------------------------------------------------------------------
# Adaptive IsolationForest
# ---------------------------------------------------------------------------

class AdaptiveAnomalyEngine:
    """
    Обертає RuleEngine + sklearn IsolationForest.
    Накопичує feature vectors, перенавчається кожні 10 хв.
    """

    WARMUP_MINUTES   = WARMUP_MINUTES
    RETRAIN_INTERVAL = RETRAIN_INTERVAL

    # Назви features для логування
    FEATURE_NAMES = [
        "kill_rate_per_min",
        "death_rate_per_hour",
        "targeting_failure_rate",
        "branch_target_duration_s",
        "branch_attack_duration_s",
        "bt_avg_us",
        "blacklist_count_10min",
        "hp_changes_per_min",
    ]

    def __init__(self, model_dir: str = MODEL_DIR):
        self.model_dir = model_dir
        os.makedirs(model_dir, exist_ok=True)

        self.rules = RuleEngine()
        self.anomaly_log: List[dict] = []

        self.feature_history: List[List[float]] = []
        self._feature_window: deque = deque()   # буфер для extract_features()
        self._event_window: deque = deque()      # (ts, event)

        self.is_trained = False
        self.session_start = time.time()
        self._last_retrain = time.time()
        self._last_feature_ts = time.time()

        if _SKLEARN_OK:
            self.clf = IsolationForest(
                n_estimators=100,
                contamination=0.05,
                random_state=42,
            )
        else:
            self.clf = None
            logger.warning("sklearn не доступний — IsolationForest вимкнено")

    # ---- ingest ------------------------------------------------------------

    def ingest_event(self, ev: dict):
        ev["_ingest_time"] = ev.get("_ingest_time") or time.time()
        self.rules.ingest_event(ev)
        self._event_window.append((ev["_ingest_time"], ev))
        # Прибираємо старіші 1 год
        cutoff = time.time() - 3600
        while self._event_window and self._event_window[0][0] < cutoff:
            self._event_window.popleft()

    def ingest_stats(self, snap: dict):
        self.rules.ingest_stats(snap)
        self._feature_window.append((time.time(), snap))
        cutoff = time.time() - 3600
        while self._feature_window and self._feature_window[0][0] < cutoff:
            self._feature_window.popleft()

    # ---- feature extraction ------------------------------------------------

    def extract_features(self, window_sec: float = 600) -> List[float]:
        """Витягує feature vector з поточного вікна."""
        now = time.time()
        cutoff = now - window_sec

        # 1. kill_rate і death_rate з stats window
        recent_stats = [(ts, s) for ts, s in self._feature_window if ts >= cutoff]

        kill_rate = 0.0
        death_rate = 0.0
        tf_rate = 0.0

        if len(recent_stats) >= 2:
            first_ts, first_s = recent_stats[0]
            last_ts, last_s = recent_stats[-1]
            elapsed_min = max((last_ts - first_ts) / 60, 0.1)
            elapsed_hr  = elapsed_min / 60

            dk = (last_s.get("kills", 0) or 0) - (first_s.get("kills", 0) or 0)
            dd = (last_s.get("deaths", 0) or 0) - (first_s.get("deaths", 0) or 0)
            dtf = (last_s.get("targeting_failures", 0) or 0) - (first_s.get("targeting_failures", 0) or 0)
            total_kills = max(last_s.get("kills", 0) or 0, 1)

            kill_rate = max(dk, 0) / elapsed_min
            death_rate = max(dd, 0) / max(elapsed_hr, 0.001)
            tf_rate = max(dtf, 0) / total_kills

        # 2. Branch durations з event window
        recent_events = [(ts, ev) for ts, ev in self._event_window if ts >= cutoff]
        target_sec = 0.0
        attack_sec = 0.0
        hp_changes = 0

        prev_ts_branch: Optional[float] = None
        prev_branch: Optional[str] = None

        for ts, ev in recent_events:
            etype = ev.get("type")
            if etype in ("bt_tick", "heartbeat"):
                branch = ev.get("branch") or ev.get("obj", "")
                if prev_branch is not None and prev_ts_branch is not None:
                    dur = ts - prev_ts_branch
                    if prev_branch == "Target":
                        target_sec += dur
                    elif prev_branch == "Attack":
                        attack_sec += dur
                prev_branch = branch
                prev_ts_branch = ts
            elif etype == "heartbeat" and ev.get("obj") == "Attack":
                hp_changes += 1  # proxy для hp changes

        # 3. BT avg
        bt_avg_us = 0.0
        if self.rules._bt_avg_us_history:
            recent = list(self.rules._bt_avg_us_history)[-50:]
            bt_avg_us = sum(recent) / len(recent)

        # 4. Blacklist count
        cutoff_5 = now - 600
        bl_count = sum(1 for ts, _ in self.rules._blacklist_events if ts >= cutoff_5)

        # 5. HP changes per min
        hp_per_min = hp_changes / max(window_sec / 60, 0.1)

        return [
            kill_rate,
            death_rate,
            tf_rate,
            target_sec,
            attack_sec,
            bt_avg_us,
            float(bl_count),
            hp_per_min,
        ]

    # ---- model -------------------------------------------------------------

    def retrain(self):
        """Re-fit IsolationForest на накопиченій history."""
        if not _SKLEARN_OK or self.clf is None:
            return
        if len(self.feature_history) < 10:
            return

        X = np.array(self.feature_history)
        # Замінюємо NaN/inf на 0
        X = np.nan_to_num(X, nan=0.0, posinf=0.0, neginf=0.0)

        try:
            self.clf.fit(X)
            self.is_trained = True
            date_str = datetime.now().strftime("%Y%m%d_%H%M%S")
            model_path = os.path.join(self.model_dir, f"model_{date_str}.pkl")
            joblib.dump(self.clf, model_path)
            logger.info(f"[QA] IsolationForest навчено на {len(X)} точках → {model_path}")
        except Exception as e:
            logger.warning(f"[QA] IsolationForest fit error: {e}")

    def predict(self, features: List[float]) -> float:
        """Повертає anomaly score 0-100 (вище = аномальніше)."""
        if not self.is_trained or self.clf is None:
            return 0.0
        try:
            arr = np.array(features, dtype=float).reshape(1, -1)
            arr = np.nan_to_num(arr, nan=0.0, posinf=0.0, neginf=0.0)
            score = -self.clf.score_samples(arr)[0]
            return float(min(max(score * 100, 0), 100))
        except Exception:
            return 0.0

    def load_previous_model(self) -> bool:
        """Завантажує останню збережену модель (для --replay режиму)."""
        if not _SKLEARN_OK:
            return False
        models = sorted(glob.glob(os.path.join(self.model_dir, "model_*.pkl")))
        if not models:
            return False
        try:
            self.clf = joblib.load(models[-1])
            self.is_trained = True
            logger.info(f"[QA] Завантажено модель: {models[-1]}")
            return True
        except Exception as e:
            logger.warning(f"[QA] Помилка завантаження моделі: {e}")
            return False

    # ---- main tick ---------------------------------------------------------

    def tick(self) -> dict:
        """
        Виклик кожен цикл (5-10с):
        - Запускає rule-based детектори
        - Якщо тепло (> WARMUP_MINUTES) — додає feature vector і predict
        - Повертає dict з total_score та списком аномалій
        """
        now = time.time()
        elapsed_min = (now - self.session_start) / 60
        warmup_done = elapsed_min >= self.WARMUP_MINUTES

        # Rule-based check
        anomalies = self.rules.check()

        # Adaptive
        adaptive_score = 0.0
        features = self.extract_features()

        if warmup_done:
            self.feature_history.append(features)
            if len(self.feature_history) > 1000:
                self.feature_history.pop(0)

            # Retrain якщо пора
            if now - self._last_retrain > self.RETRAIN_INTERVAL:
                self.retrain()
                self._last_retrain = now

            adaptive_score = self.predict(features)

        # Total score
        rule_total = sum(a["score"] for a in anomalies)
        total_score = rule_total + adaptive_score * 20 / 100 * 20
        # clamp
        total_score = min(total_score, 150)

        # Додаємо ts до аномалій
        ts_str = datetime.now().strftime("%H:%M:%S")
        for a in anomalies:
            a["ts"] = ts_str
            a["timestamp"] = now

        # Логуємо в history
        if anomalies:
            self.anomaly_log.extend(anomalies)
            if len(self.anomaly_log) > 500:
                self.anomaly_log = self.anomaly_log[-500:]

        return {
            "total_score": total_score,
            "rule_score": rule_total,
            "adaptive_score": adaptive_score,
            "anomalies": anomalies,
            "features": features,
            "warmup_done": warmup_done,
            "elapsed_min": elapsed_min,
            "ts": ts_str,
        }

    # ---- analyze mode -------------------------------------------------------

    def analyze_stats_records(self, records: List[dict]) -> List[dict]:
        """
        Аналізує список stats записів (режим --analyze-only).
        Повертає список знайдених аномалій.
        """
        from datetime import datetime as dt

        all_anomalies = []
        kills_10min_start = (0, None)   # (kills, ts_float)

        for rec in records:
            snap = {
                "kills": rec.get("kills", 0),
                "deaths": rec.get("deaths", 0),
                "attacks": rec.get("attacks", 0),
                "targeting_failures": rec.get("targeting_failures", 0),
                "uptime_sec": rec.get("uptime_sec", 0),
            }
            ts_dt = rec.get("timestamp_dt")
            ts_float = ts_dt.timestamp() if ts_dt else time.time()

            # Ін'єктуємо ts для live_stats stale checker
            self.rules._live_stats_last_ts = None  # не перевіряємо live в analyze mode

            self.rules.ingest_stats(snap, ts=ts_float)
            self._feature_window.append((ts_float, snap))

            anomalies = self.rules.check()

            # no_kills_10min
            if kills_10min_start[1] is not None:
                elapsed_min = (ts_float - kills_10min_start[1]) / 60
                no_kill_anomalies = self.rules.check_no_kills_window(
                    snap["kills"], kills_10min_start[0], elapsed_min
                )
                anomalies.extend(no_kill_anomalies)

            if kills_10min_start[1] is None:
                kills_10min_start = (snap["kills"], ts_float)
            elif (ts_float - kills_10min_start[1]) > 600:
                kills_10min_start = (snap["kills"], ts_float)

            ts_str = ts_dt.strftime("%H:%M:%S") if ts_dt else "??:??:??"
            for a in anomalies:
                a["ts"] = ts_str
                a["timestamp"] = ts_float
                all_anomalies.append(a)

        # Feature vector для adaptive
        features = self.extract_features()
        self.feature_history.append(features)

        return all_anomalies

    # ---- properties --------------------------------------------------------

    @property
    def recent_anomalies(self) -> List[dict]:
        """Останні 20 аномалій."""
        return self.anomaly_log[-20:]

    def score_history_for_dashboard(self, n: int = 60) -> List[dict]:
        """Заглушка — в daemon режимі додавати через окремий tracker."""
        return []
