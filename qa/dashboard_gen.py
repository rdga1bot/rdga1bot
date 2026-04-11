"""
dashboard_gen.py — генератор qa/dashboard.html з Chart.js (CDN).
Оновлюється кожні 60с через перезапис файлу.
"""

import os
import json
import html
from datetime import datetime
from typing import List, Optional, Dict, Any

DASHBOARD_PATH = os.path.join(os.path.dirname(__file__), "dashboard.html")

SEVERITY_COLORS = {
    "INFO":     ("#4CAF50", "success"),
    "WARNING":  ("#FF9800", "warning"),
    "ERROR":    ("#F44336", "danger"),
    "CRITICAL": ("#9C27B0", "info"),
}

BRANCH_COLORS = {
    "Dead":    "#e53935",
    "Rest":    "#fbc02d",
    "Zone":    "#8d6e63",
    "Buff":    "#7b1fa2",
    "Loot":    "#1976d2",
    "Attack":  "#388e3c",
    "Target":  "#0288d1",
    "None":    "#9e9e9e",
}


def _score_color(score: float) -> str:
    if score > 80:
        return "#e53935"
    elif score > 50:
        return "#fb8c00"
    elif score > 30:
        return "#fdd835"
    else:
        return "#43a047"


def _esc(s: str) -> str:
    return html.escape(str(s))


class DashboardGen:
    """Генерує dashboard.html на основі поточного стану QA системи."""

    def __init__(self, output_path: str = DASHBOARD_PATH):
        self.output_path = output_path
        self._score_history: List[dict] = []    # [{ts, score}, ...]
        self._kill_rate_history: List[dict] = [] # [{ts, rate}, ...]
        self._branch_timeline: List[dict] = []   # [{ts, branch, duration_s}]
        self._current_branch: Optional[str] = None
        self._branch_start: Optional[float] = None

    def push_score(self, ts: str, score: float):
        self._score_history.append({"ts": ts, "score": round(score, 1)})
        if len(self._score_history) > 120:
            self._score_history.pop(0)

    def push_kill_rate(self, ts: str, rate: float):
        self._kill_rate_history.append({"ts": ts, "rate": round(rate, 2)})
        if len(self._kill_rate_history) > 120:
            self._kill_rate_history.pop(0)

    def push_branch(self, branch: str):
        """Записує зміну branch для timeline."""
        import time
        now = time.time()
        if self._current_branch and self._branch_start:
            dur = now - self._branch_start
            self._branch_timeline.append({
                "branch": self._current_branch,
                "start": self._branch_start,
                "duration": dur,
                "ts": datetime.fromtimestamp(self._branch_start).strftime("%H:%M:%S"),
            })
            if len(self._branch_timeline) > 200:
                self._branch_timeline.pop(0)
        self._current_branch = branch
        self._branch_start = now

    def render(self,
               session_name: str,
               total_score: float,
               uptime_str: str,
               anomalies: List[dict],
               live_stats: Optional[dict] = None,
               frames: Optional[List[dict]] = None):
        """Генерує та зберігає dashboard.html."""

        now_str = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        score_color = _score_color(total_score)

        # Визначити severity label
        if total_score > 80:
            severity_label = "CRITICAL"
        elif total_score > 50:
            severity_label = "ALERT"
        elif total_score > 30:
            severity_label = "WARNING"
        else:
            severity_label = "OK"

        html_content = self._build_html(
            session_name=session_name,
            now_str=now_str,
            total_score=total_score,
            score_color=score_color,
            severity_label=severity_label,
            uptime_str=uptime_str,
            anomalies=anomalies,
            live_stats=live_stats or {},
            frames=frames or [],
        )

        tmp_path = self.output_path + ".tmp"
        with open(tmp_path, "w", encoding="utf-8") as fh:
            fh.write(html_content)
        os.replace(tmp_path, self.output_path)

    def _build_html(self, **ctx) -> str:
        session_name = _esc(ctx["session_name"])
        now_str      = _esc(ctx["now_str"])
        total_score  = ctx["total_score"]
        score_color  = ctx["score_color"]
        severity_label = ctx["severity_label"]
        uptime_str   = _esc(ctx["uptime_str"])
        anomalies    = ctx["anomalies"]
        live_stats   = ctx["live_stats"]
        frames       = ctx["frames"]

        # Chart.js data
        score_labels = json.dumps([d["ts"] for d in self._score_history])
        score_data   = json.dumps([d["score"] for d in self._score_history])
        kill_labels  = json.dumps([d["ts"] for d in self._kill_rate_history])
        kill_data    = json.dumps([d["rate"] for d in self._kill_rate_history])

        # Branch timeline (горизонтальні смуги)
        branch_rows = self._build_branch_timeline_html()

        # Anomaly table
        anomaly_rows = self._build_anomaly_table_html(anomalies)

        # Live stats
        live_html = self._build_live_stats_html(live_stats)

        # Frames
        frames_html = self._build_frames_html(frames)

        return f"""<!DOCTYPE html>
<html lang="uk">
<head>
  <meta charset="UTF-8">
  <meta http-equiv="refresh" content="60">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>rdga1bot QA Dashboard</title>
  <script src="https://cdn.jsdelivr.net/npm/chart.js@4.4.0/dist/chart.umd.min.js"></script>
  <style>
    * {{ box-sizing: border-box; margin: 0; padding: 0; }}
    body {{ background: #1a1a2e; color: #e0e0e0; font-family: 'Courier New', monospace; padding: 12px; }}
    h1 {{ color: #90caf9; margin-bottom: 4px; }}
    h2 {{ color: #64b5f6; font-size: 0.95em; margin: 12px 0 6px; border-bottom: 1px solid #333; padding-bottom: 4px; }}
    .header {{ display: flex; align-items: center; gap: 20px; margin-bottom: 16px; background: #16213e; padding: 12px; border-radius: 8px; }}
    .score-badge {{ font-size: 2.8em; font-weight: bold; color: {score_color}; text-shadow: 0 0 8px {score_color}; }}
    .meta {{ font-size: 0.8em; color: #9e9e9e; }}
    .grid {{ display: grid; grid-template-columns: 1fr 1fr; gap: 12px; }}
    .card {{ background: #16213e; border-radius: 8px; padding: 12px; }}
    .full-width {{ grid-column: 1 / -1; }}
    canvas {{ max-height: 180px; }}
    table {{ width: 100%; border-collapse: collapse; font-size: 0.78em; }}
    th {{ background: #0f3460; padding: 6px 8px; text-align: left; }}
    td {{ padding: 5px 8px; border-bottom: 1px solid #222; }}
    tr:hover td {{ background: #1e3a5f; }}
    .sev-INFO     {{ color: #4CAF50; }}
    .sev-WARNING  {{ color: #FF9800; }}
    .sev-ERROR    {{ color: #F44336; }}
    .sev-CRITICAL {{ color: #e040fb; }}
    .branch-bar {{ display: flex; height: 24px; border-radius: 4px; overflow: hidden; margin: 4px 0; }}
    .branch-seg {{ height: 100%; display: flex; align-items: center; font-size: 0.65em; overflow: hidden; padding: 0 2px; color: #fff; }}
    .frames-grid {{ display: grid; grid-template-columns: repeat(auto-fill, minmax(200px, 1fr)); gap: 8px; }}
    .frame-thumb {{ border-radius: 4px; overflow: hidden; border: 1px solid #333; }}
    .frame-thumb img {{ width: 100%; display: block; }}
    .frame-name {{ font-size: 0.7em; color: #9e9e9e; text-align: center; padding: 2px; }}
    .stat-grid {{ display: grid; grid-template-columns: repeat(auto-fill, minmax(140px, 1fr)); gap: 8px; }}
    .stat-box {{ background: #0f3460; border-radius: 6px; padding: 8px; text-align: center; }}
    .stat-val {{ font-size: 1.6em; font-weight: bold; color: #90caf9; }}
    .stat-lbl {{ font-size: 0.7em; color: #9e9e9e; }}
    .ok {{ color: #43a047; }} .warn {{ color: #fb8c00; }} .crit {{ color: #e53935; }}
  </style>
</head>
<body>

<!-- 1. Header -->
<div class="header">
  <div>
    <div class="score-badge">{total_score:.0f}</div>
    <div class="meta" style="color:{score_color}"><b>{severity_label}</b></div>
  </div>
  <div>
    <h1>rdga1bot QA Dashboard</h1>
    <div class="meta">Сесія: {session_name}</div>
    <div class="meta">Оновлено: {now_str}</div>
    <div class="meta">Uptime: {uptime_str}</div>
  </div>
</div>

<div class="grid">

  <!-- 2. BT Branch Timeline -->
  <div class="card full-width">
    <h2>BT Branch Timeline (останні події)</h2>
    {branch_rows}
  </div>

  <!-- 3. Anomaly Score Graph -->
  <div class="card">
    <h2>Anomaly Score (останні 60 хв)</h2>
    <canvas id="scoreChart"></canvas>
  </div>

  <!-- 4. Kill Rate Graph -->
  <div class="card">
    <h2>Kill Rate / хв</h2>
    <canvas id="killChart"></canvas>
  </div>

  <!-- 6. Live Stats -->
  <div class="card">
    <h2>Live Stats</h2>
    {live_html}
  </div>

  <!-- 5. Anomaly Table -->
  <div class="card">
    <h2>Anomaly Log (останні 20)</h2>
    {anomaly_rows}
  </div>

  <!-- 7. Recent Frames -->
  <div class="card full-width">
    <h2>Recent Frames</h2>
    {frames_html}
  </div>

</div>

<script>
// Score chart
new Chart(document.getElementById('scoreChart'), {{
  type: 'line',
  data: {{
    labels: {score_labels},
    datasets: [{{
      label: 'Anomaly Score',
      data: {score_data},
      borderColor: '#ef5350',
      backgroundColor: 'rgba(239,83,80,0.15)',
      tension: 0.3,
      fill: true,
    }}]
  }},
  options: {{
    animation: false,
    plugins: {{ legend: {{ display: false }} }},
    scales: {{
      y: {{ min: 0, max: 100, grid: {{ color: '#333' }}, ticks: {{ color: '#9e9e9e' }} }},
      x: {{ grid: {{ color: '#2a2a2a' }}, ticks: {{ color: '#9e9e9e', maxTicksLimit: 8 }} }}
    }}
  }}
}});

// Kill rate chart
new Chart(document.getElementById('killChart'), {{
  type: 'line',
  data: {{
    labels: {kill_labels},
    datasets: [{{
      label: 'kills/хв',
      data: {kill_data},
      borderColor: '#66bb6a',
      backgroundColor: 'rgba(102,187,106,0.15)',
      tension: 0.3,
      fill: true,
    }}]
  }},
  options: {{
    animation: false,
    plugins: {{ legend: {{ display: false }} }},
    scales: {{
      y: {{ min: 0, grid: {{ color: '#333' }}, ticks: {{ color: '#9e9e9e' }} }},
      x: {{ grid: {{ color: '#2a2a2a' }}, ticks: {{ color: '#9e9e9e', maxTicksLimit: 8 }} }}
    }}
  }}
}});
</script>
</body>
</html>
"""

    def _build_branch_timeline_html(self) -> str:
        if not self._branch_timeline:
            return '<div style="color:#9e9e9e;font-size:0.8em">Дані відсутні (daemon не запущений або немає branch подій)</div>'

        recent = self._branch_timeline[-60:]
        total_dur = sum(b["duration"] for b in recent)
        if total_dur < 0.1:
            return '<div style="color:#9e9e9e">Не вистачає даних</div>'

        segs = ""
        for b in recent:
            pct = b["duration"] / total_dur * 100
            if pct < 0.5:
                continue
            color = BRANCH_COLORS.get(b["branch"], "#607d8b")
            label = b["branch"][:3] if pct < 3 else b["branch"]
            title = f'{b["branch"]} {b["ts"]} ({b["duration"]:.0f}s)'
            segs += (f'<div class="branch-seg" style="width:{pct:.1f}%;background:{color}" '
                     f'title="{_esc(title)}">{_esc(label)}</div>')

        legend = "".join(
            f'<span style="background:{c};padding:2px 6px;border-radius:3px;margin:2px;font-size:0.7em">{_esc(b)}</span>'
            for b, c in BRANCH_COLORS.items() if b != "None"
        )

        return f'<div class="branch-bar">{segs}</div><div style="margin-top:4px">{legend}</div>'

    def _build_anomaly_table_html(self, anomalies: List[dict]) -> str:
        if not anomalies:
            return '<div style="color:#9e9e9e;font-size:0.8em">Аномалій не виявлено</div>'

        rows = ""
        for a in reversed(anomalies[-20:]):
            sev = a.get("severity", "INFO")
            name = _esc(a.get("name", ""))
            score = a.get("score", 0)
            ctx = _esc(a.get("context", "")[:80])
            ts = _esc(a.get("ts", ""))
            rows += f'<tr><td>{ts}</td><td class="sev-{sev}">{sev}</td><td>{name}</td><td>{score}</td><td>{ctx}</td></tr>'

        return f"""<table>
<thead><tr><th>Час</th><th>Severity</th><th>Детектор</th><th>Score</th><th>Контекст</th></tr></thead>
<tbody>{rows}</tbody>
</table>"""

    def _build_live_stats_html(self, live: dict) -> str:
        if not live.get("available", True) or not live:
            available_note = ""
            if not live.get("available", False):
                available_note = '<div style="color:#9e9e9e;font-size:0.8em">⚠ /tmp/rdga1bot_stats.json не знайдено.<br>Запусти бота або додай live stats export в Stats.cpp.</div>'

        kills  = live.get("kills", "—")
        deaths = live.get("deaths", "—")
        attacks = live.get("attacks", "—")
        tf     = live.get("targeting_failures", "—")
        uptime = live.get("uptime_sec", 0)
        age    = live.get("age_sec")
        stale  = live.get("stale", False)

        if isinstance(uptime, (int, float)) and uptime > 0:
            uptime_str = f"{uptime//3600:.0f}h {(uptime%3600)//60:.0f}m {uptime%60:.0f}s"
        else:
            uptime_str = "—"

        age_html = ""
        if age is not None:
            color = "#e53935" if stale else "#43a047"
            age_html = f'<div style="color:{color};font-size:0.75em">Вік файлу: {age:.0f}с {"⚠ STALE" if stale else "✓"}</div>'

        avail_note = ""
        if not live.get("available", True):
            avail_note = '<div style="color:#ff9800;font-size:0.75em">⚠ /tmp/rdga1bot_stats.json недоступний</div>'

        return f"""
{avail_note}
<div class="stat-grid">
  <div class="stat-box"><div class="stat-val">{kills}</div><div class="stat-lbl">Kills</div></div>
  <div class="stat-box"><div class="stat-val">{deaths}</div><div class="stat-lbl">Deaths</div></div>
  <div class="stat-box"><div class="stat-val">{attacks}</div><div class="stat-lbl">Attacks</div></div>
  <div class="stat-box"><div class="stat-val">{tf}</div><div class="stat-lbl">TF</div></div>
  <div class="stat-box"><div class="stat-val" style="font-size:1em">{uptime_str}</div><div class="stat-lbl">Uptime</div></div>
</div>
{age_html}
"""

    def _build_frames_html(self, frames: List[dict]) -> str:
        if not frames:
            return '<div style="color:#9e9e9e;font-size:0.8em">Знімків немає (scrot не встановлено або daemon не запущений)</div>'

        items = ""
        for f in frames:
            b64 = f.get("b64", "")
            fname = _esc(f.get("filename", ""))
            if b64:
                items += f'''<div class="frame-thumb">
  <img src="data:image/png;base64,{b64}" alt="{fname}">
  <div class="frame-name">{fname}</div>
</div>'''
        return f'<div class="frames-grid">{items}</div>'
