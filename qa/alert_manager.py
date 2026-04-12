"""
alert_manager.py — notify-send + terminal bell + log alerts.

Правила:
  score > 30  → WARNING (тільки dashboard)
  score > 50  → notify-send алерт + запис в qa/alerts.log
  score > 80  → CRITICAL алерт + terminal bell (\a)
"""

import os
import subprocess
import logging
from datetime import datetime
from typing import Optional

logger = logging.getLogger("qa.alert")

ALERT_LOG = os.path.join(os.path.dirname(__file__), "alerts.log")

SEVERITY_COLORS = {
    "INFO":     "#4CAF50",
    "WARNING":  "#FF9800",
    "ERROR":    "#F44336",
    "CRITICAL": "#9C27B0",
}

_NOTIFY_AVAILABLE: Optional[bool] = None


def _check_notify() -> bool:
    global _NOTIFY_AVAILABLE
    if _NOTIFY_AVAILABLE is None:
        _NOTIFY_AVAILABLE = subprocess.run(
            ["which", "notify-send"], capture_output=True
        ).returncode == 0
    return _NOTIFY_AVAILABLE


class AlertManager:
    """Керує відправкою алертів різними каналами."""

    def __init__(self, alert_log: str = ALERT_LOG):
        self.alert_log = alert_log
        self._cooldowns: dict = {}   # name → last_alert_time (не спамити)
        self.COOLDOWN_SEC = 300       # не повторювати той самий алерт < 5 хв

        # Глобальний rate limit: не більше MAX_NOTIFY_PER_WINDOW нотифікацій
        # за NOTIFY_WINDOW_SEC секунд (захист від спаму при багатьох аномаліях одразу)
        self.MAX_NOTIFY_PER_WINDOW = 3
        self.NOTIFY_WINDOW_SEC     = 300
        self._notify_window_start: float = 0.0
        self._notify_count_in_window: int = 0

    def _cooldown_ok(self, name: str) -> bool:
        import time
        last = self._cooldowns.get(name, 0)
        return (time.time() - last) > self.COOLDOWN_SEC

    def _mark_cooldown(self, name: str):
        import time
        self._cooldowns[name] = time.time()

    def _global_notify_ok(self) -> bool:
        """Перевіряє глобальний rate limit на notify-send."""
        import time
        now = time.time()
        if now - self._notify_window_start > self.NOTIFY_WINDOW_SEC:
            self._notify_window_start    = now
            self._notify_count_in_window = 0
        if self._notify_count_in_window >= self.MAX_NOTIFY_PER_WINDOW:
            return False
        self._notify_count_in_window += 1
        return True

    def alert(self, anomaly: dict, total_score: float):
        """Відправляє алерт на основі severity та score."""
        name     = anomaly.get("name", "unknown")
        score    = anomaly.get("score", 0)
        severity = anomaly.get("severity", "WARNING")
        context  = anomaly.get("context", "")
        ts       = anomaly.get("ts", datetime.now().strftime("%H:%M:%S"))

        # Завжди логуємо в файл якщо score > 50
        if total_score > 50:
            self._write_log(ts, name, score, severity, context)

        if not self._cooldown_ok(name):
            return

        if total_score > 80:
            self._critical_alert(name, score, severity, context)
            self._mark_cooldown(name)
        elif total_score > 50:
            self._send_notify(name, score, severity, context)
            self._mark_cooldown(name)

    def _critical_alert(self, name: str, score: float, severity: str, context: str):
        """CRITICAL: notify-send + terminal bell."""
        print('\a', end='', flush=True)  # terminal bell
        logger.critical(f"[ALERT] CRITICAL: {name} score={score:.0f} | {context}")
        self._send_notify(name, score, severity, context, urgency="critical")

    def _send_notify(self, name: str, score: float, severity: str, context: str,
                     urgency: str = "normal"):
        """Відправляє notify-send toast з глобальним rate limit."""
        if not _check_notify():
            return
        if not self._global_notify_ok():
            logger.debug(f"[AlertManager] notify rate-limit: пропускаємо {name}")
            return
        try:
            title = f"rdga1bot QA: {severity} [{score:.0f}]"
            body = f"{name}\n{context}"
            subprocess.Popen(
                ["notify-send",
                 "-u", urgency,
                 "-t", "8000",
                 title, body],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
        except Exception as e:
            logger.debug(f"notify-send error: {e}")

    def _write_log(self, ts: str, name: str, score: float, severity: str, context: str):
        """Запис в qa/alerts.log."""
        line = f"{datetime.now().isoformat()} [{severity}] {name} score={score:.0f} | {context}\n"
        try:
            with open(self.alert_log, "a", encoding="utf-8") as fh:
                fh.write(line)
        except OSError:
            pass

    def alert_many(self, anomalies: list, total_score: float):
        """Відправляє алерти для списку аномалій.
        Логує кожен у файл, але шле не більше 1 notify-send на весь пакет
        (групує в одне повідомлення щоб не спамити робочий стіл).
        """
        if not anomalies:
            return

        # Завжди пишемо у файл/термінал
        for a in anomalies:
            name     = a.get("name", "unknown")
            score    = a.get("score", 0)
            severity = a.get("severity", "WARNING")
            context  = a.get("context", "")
            ts       = a.get("ts", datetime.now().strftime("%H:%M:%S"))
            if total_score > 50:
                self._write_log(ts, name, score, severity, context)
            if total_score > 80:
                logger.critical(f"[ALERT] CRITICAL: {name} score={score:.0f} | {context}")
            elif total_score > 50:
                logger.warning(f"[ALERT] WARNING: {name} score={score:.0f} | {context}")

        # Одна групована нотифікація на весь пакет (rate-limit + cooldown)
        if total_score <= 50 or not _check_notify():
            return
        # Перевіряємо чи хоча б одна аномалія пройшла per-name cooldown
        any_new = any(self._cooldown_ok(a.get("name", "?")) for a in anomalies)
        if not any_new:
            return
        if not self._global_notify_ok():
            return

        worst = max(anomalies, key=lambda a: a.get("score", 0))
        extra = len(anomalies) - 1
        severity = worst.get("severity", "WARNING")
        urgency  = "critical" if total_score > 80 else "normal"
        if total_score > 80:
            print('\a', end='', flush=True)
        title = f"rdga1bot QA: {severity} [{total_score:.0f}]"
        body  = f"{worst.get('name','?')}: {worst.get('context','')}"
        if extra > 0:
            body += f"\n(+{extra} більше аномалій)"
        try:
            subprocess.Popen(
                ["notify-send", "-u", urgency, "-t", "10000", title, body],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            )
        except Exception as e:
            logger.debug(f"notify-send error: {e}")
        # Позначаємо cooldown для всіх
        for a in anomalies:
            self._mark_cooldown(a.get("name", "?"))
