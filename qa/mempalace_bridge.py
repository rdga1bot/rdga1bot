"""Міст між QA Monitor та MemPalace.

Graceful degradation: якщо MemPalace не ініціалізований або venv відсутній —
QA Monitor продовжує без нього. Всі публічні методи безпечні навіть без palace.

MemPalace CLI викликається через venv python щоб уникнути несумісності
chromadb/pydantic з Python 3.14 (системний). Venv: memory/venv (Python 3.12).
"""

import json
import re
import subprocess
import time
from datetime import datetime
from pathlib import Path
from typing import Optional


class MemPalaceBridge:
    def __init__(self, project_root: str = "."):
        self.project_root  = Path(project_root).resolve()
        self.palace_path   = self.project_root / "memory" / "palace"
        self.venv_mp       = self.project_root / "memory" / "venv" / "bin" / "mempalace"
        self.anomalies_file = self.project_root / "qa" / "anomalies.jsonl"

        # Sentinel + venv обидва мають існувати
        sentinel = self.project_root / "memory" / ".mempalace_initialized"
        self.enabled = sentinel.exists() and self.venv_mp.exists()

        if self.enabled:
            print(f"[MemPalace] Bridge активний. Palace: {self.palace_path}")
        else:
            if not sentinel.exists():
                print("[MemPalace] Bridge вимкнено (memory/.mempalace_initialized не знайдено)")
            if not self.venv_mp.exists():
                print(f"[MemPalace] Bridge вимкнено (venv mempalace не знайдено: {self.venv_mp})")

    # ── Запис аномалій ────────────────────────────────────────────────────────

    def log_anomaly(self, anomaly: dict):
        """Записує аномалію у qa/anomalies.jsonl. Працює без MemPalace."""
        record = {
            "timestamp": datetime.now().isoformat(),
            "type": "anomaly",
            **anomaly,
        }
        try:
            with open(self.anomalies_file, "a", encoding="utf-8") as fh:
                fh.write(json.dumps(record, ensure_ascii=False) + "\n")
        except Exception as e:
            print(f"[MemPalace] Помилка запису аномалії: {e}")

    # ── Пошук ─────────────────────────────────────────────────────────────────

    def search(self, query: str, wing: str = "", results: int = 3) -> str:
        """Пошук у MemPalace. Повертає plain text (немає --json в CLI)."""
        if not self.enabled:
            return ""
        cmd = [str(self.venv_mp), "--palace", str(self.palace_path),
               "search", query, "--results", str(results)]
        if wing:
            cmd += ["--wing", wing]
        try:
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=8)
            return r.stdout if r.returncode == 0 else ""
        except Exception:
            return ""

    def get_baseline_kill_rate(self, zone: str = "ElmoreLab") -> Optional[float]:
        """Отримує baseline kill rate з MemPalace через семантичний пошук."""
        if not self.enabled:
            return None
        output = self.search(f"kills per minute kill rate {zone}", wing="rdga1bot-sessions",
                             results=5)
        if not output:
            # fallback: пошук у всіх wings
            output = self.search(f"kill rate kills per minute", results=5)
        if not output:
            return None
        # Парсимо числа типу "23.54/хв", "kill_rate=12.5", "13.0 kill/min"
        patterns = [
            r'kill[_\s]rate[_\s=:]+([0-9]+\.?[0-9]*)',
            r'([0-9]+\.?[0-9]*)\s*/\s*(?:хв|min)',
            r'([0-9]+\.?[0-9]*)\s+kill[s]?/min',
        ]
        for pat in patterns:
            m = re.search(pat, output, re.I)
            if m:
                try:
                    val = float(m.group(1))
                    if 0.5 < val < 200:   # розумний діапазон
                        return val
                except ValueError:
                    pass
        return None

    def get_wake_up_context(self, wing: str = "rdga1bot-src") -> str:
        """Wake-up контекст (~600 токенів) для Claude Code."""
        if not self.enabled:
            return ""
        try:
            r = subprocess.run(
                [str(self.venv_mp), "--palace", str(self.palace_path),
                 "wake-up", "--wing", wing],
                capture_output=True, text=True, timeout=10,
            )
            return r.stdout if r.returncode == 0 else ""
        except Exception:
            return ""

    def trigger_mining(self):
        """Запускає mempalace-mine.sh асинхронно (після завершення сесії)."""
        if not self.enabled:
            return
        mine_script = self.project_root / "scripts" / "mempalace-mine.sh"
        if mine_script.exists():
            subprocess.Popen(
                [str(mine_script)],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            print("[MemPalace] Запущено фінальний mining")
        else:
            print(f"[MemPalace] mine script не знайдено: {mine_script}")
