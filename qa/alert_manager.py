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
        self.COOLDOWN_SEC = 120       # не повторювати той самий алерт < 2 хв

    def _cooldown_ok(self, name: str) -> bool:
        import time
        last = self._cooldowns.get(name, 0)
        return (time.time() - last) > self.COOLDOWN_SEC

    def _mark_cooldown(self, name: str):
        import time
        self._cooldowns[name] = time.time()

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
        msg = f"[CRITICAL] rdga1bot anomaly: {name}\n{context}"
        logger.critical(f"[ALERT] CRITICAL: {name} score={score:.0f} | {context}")
        self._send_notify(name, score, severity, context, urgency="critical")

    def _send_notify(self, name: str, score: float, severity: str, context: str,
                     urgency: str = "normal"):
        """Відправляє notify-send toast."""
        if not _check_notify():
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
        """Відправляє алерти для списку аномалій."""
        for a in anomalies:
            self.alert(a, total_score)
