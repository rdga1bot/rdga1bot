#!/usr/bin/env python3
"""
navmesh_preview.py — візуалізація navmesh_points.pts
Формат .pts: uint32_t count + count × (float x, float y, float z)

Використання:
  python3 scripts/navmesh_preview.py [pts_file] [output_png]

Приклади:
  python3 scripts/navmesh_preview.py
  python3 scripts/navmesh_preview.py navmesh_points.pts
  python3 scripts/navmesh_preview.py navmesh_points.pts navmesh_preview.png
"""

import struct
import sys
import os


def read_pts(path):
    import math
    with open(path, "rb") as f:
        cnt_bytes = f.read(4)
        if len(cnt_bytes) < 4:
            raise ValueError(f"Файл '{path}' порожній або пошкоджений")
        cnt = struct.unpack_from("<I", cnt_bytes)[0]
        raw = f.read(cnt * 12)
        if len(raw) < cnt * 12:
            actual = len(raw) // 12
            print(f"[WARN] Очікувалось {cnt} точок, прочитано {actual}")
            cnt = actual
        pts = struct.unpack_from(f"<{cnt * 3}f", raw)

    # Фільтр: відкидаємо (0,0,0), NaN/inf, надто великі, та "origin junk"
    # Origin junk = точки де |x|+|y| < 50000 та |z| < 50000 (bad memory reads)
    WORLD_LIMIT = 400_000.0
    ORIGIN_MIN  = 50_000.0   # реальні L2 зони фарму далі ніж 50k від 0,0
    xs, ys, zs = [], [], []
    skipped = 0
    for i in range(cnt):
        x, y, z = pts[i*3], pts[i*3+1], pts[i*3+2]
        if x == 0.0 and y == 0.0 and z == 0.0:
            skipped += 1; continue
        if not (math.isfinite(x) and math.isfinite(y) and math.isfinite(z)):
            skipped += 1; continue
        if abs(x) > WORLD_LIMIT or abs(y) > WORLD_LIMIT or abs(z) > WORLD_LIMIT:
            skipped += 1; continue
        # Відкидаємо артефакти поганого memory read (дуже малі координати)
        if abs(x) + abs(y) < ORIGIN_MIN:
            skipped += 1; continue
        xs.append(x); ys.append(y); zs.append(z)
    if skipped:
        print(f"[WARN] Відфільтровано {skipped} некоректних точок (з {cnt})")
    return xs, ys, zs, len(xs)


def main():
    pts_file = sys.argv[1] if len(sys.argv) > 1 else "navmesh_points.pts"
    out_file = sys.argv[2] if len(sys.argv) > 2 else "navmesh_preview.png"

    if not os.path.exists(pts_file):
        print(f"[ERR] Файл не знайдено: {pts_file}")
        sys.exit(1)

    xs, ys, zs, cnt = read_pts(pts_file)
    print(f"[OK] Завантажено {cnt} точок з '{pts_file}'")

    if cnt == 0:
        print("[WARN] 0 точок — нема чого малювати")
        sys.exit(0)

    # Статистика
    min_x, max_x = min(xs), max(xs)
    min_y, max_y = min(ys), max(ys)
    min_z, max_z = min(zs), max(zs)
    w = max_x - min_x
    h = max_y - min_y
    print(f"     X: {min_x:.0f} … {max_x:.0f}  (ширина {w:.0f} L2u)")
    print(f"     Y: {min_y:.0f} … {max_y:.0f}  (висота {h:.0f} L2u)")
    print(f"     Z: {min_z:.0f} … {max_z:.0f}")

    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        import matplotlib.cm as cm
    except ImportError:
        print("[ERR] matplotlib не встановлено: pip install matplotlib")
        sys.exit(1)

    # --- Знаходимо головний кластер (найщільніша 30k×30k зона) ---
    from collections import Counter
    CELL = 5000
    zones = Counter((int(x / CELL), int(y / CELL)) for x, y in zip(xs, ys))
    best_cell, best_cnt = zones.most_common(1)[0]
    # Розширюємо ±2 клітинки навколо найщільнішої
    cx_min = (best_cell[0] - 2) * CELL
    cx_max = (best_cell[0] + 3) * CELL
    cy_min = (best_cell[1] - 2) * CELL
    cy_max = (best_cell[1] + 3) * CELL
    mask = [cx_min <= x <= cx_max and cy_min <= y <= cy_max
            for x, y in zip(xs, ys)]
    xs_c = [x for x, m in zip(xs, mask) if m]
    ys_c = [y for y, m in zip(ys, mask) if m]
    zs_c = [z for z, m in zip(zs, mask) if m]
    main_cnt = len(xs_c)

    fig, axes = plt.subplots(1, 3, figsize=(21, 7))
    fig.patch.set_facecolor("#1a1a2e")

    def style_ax(a, title):
        a.set_facecolor("#0f0f1a")
        a.set_title(title, color="white", fontsize=10)
        a.tick_params(colors="#888888", labelsize=7)
        a.spines[:].set_color("#333355")
        a.set_xlabel("X (L2u)", color="#aaaacc", fontsize=8)
        a.set_ylabel("Y (L2u)", color="#aaaacc", fontsize=8)

    # --- [0] Загальний вид — всі точки ---
    ax0 = axes[0]
    ax0.scatter(xs, ys, c="#4fc3f7", s=1.0, alpha=0.4, linewidths=0)
    # Позначаємо головний кластер рамкою
    from matplotlib.patches import Rectangle
    rect = Rectangle((cx_min, cy_min), cx_max - cx_min, cy_max - cy_min,
                      linewidth=1, edgecolor="#ff6b6b", facecolor="none", linestyle="--")
    ax0.add_patch(rect)
    ax0.set_aspect("equal")
    style_ax(ax0, f"Всі точки: {cnt}\nArea: {w:.0f}×{h:.0f} L2u")

    # --- [1] Зум на головний кластер ---
    ax1 = axes[1]
    if xs_c:
        sc = ax1.scatter(xs_c, ys_c, c=zs_c, cmap="plasma", s=3.0, alpha=0.8, linewidths=0)
        plt.colorbar(sc, ax=ax1, label="Z", shrink=0.8)
        cw = cx_max - cx_min
        ch = cy_max - cy_min
        ax1.set_aspect("equal")
        style_ax(ax1, f"Головний кластер: {main_cnt} pts\n"
                      f"X={cx_min:.0f}…{cx_max:.0f} Y={cy_min:.0f}…{cy_max:.0f}")
    else:
        ax1.text(0.5, 0.5, "Немає точок", color="white", ha="center", va="center")
        style_ax(ax1, "Головний кластер")

    # --- [2] Профіль висот X/Z (головний кластер) ---
    ax2 = axes[2]
    if xs_c:
        ax2.scatter(xs_c, zs_c, c="#81c784", s=2.0, alpha=0.6, linewidths=0)
        z_min_c = min(zs_c); z_max_c = max(zs_c)
        ax2.set_ylabel("Z (висота)", color="#aaaacc", fontsize=8)
    else:
        z_min_c = z_max_c = 0
    ax2.set_facecolor("#0f0f1a")
    ax2.set_title(f"Профіль X/Z (головний кластер)\nZ: {z_min_c:.0f}…{z_max_c:.0f} L2u",
                  color="white", fontsize=10)
    ax2.tick_params(colors="#888888", labelsize=7)
    ax2.spines[:].set_color("#333355")
    ax2.set_xlabel("X (L2u)", color="#aaaacc", fontsize=8)

    fig.suptitle(f"rdga1bot NavMesh  |  {pts_file}  |  {cnt} points",
                 color="#e0e0ff", fontsize=12)
    plt.tight_layout()

    plt.savefig(out_file, dpi=150, bbox_inches="tight",
                facecolor=fig.get_facecolor())
    plt.close()
    print(f"[OK] Збережено: {out_file}")
    print(f"     Головний кластер: {main_cnt} точок  "
          f"X={cx_min:.0f}…{cx_max:.0f}  Y={cy_min:.0f}…{cy_max:.0f}")

    # Відкрити одразу (якщо є viewer)
    try:
        import subprocess
        viewer = None
        for v in ("feh", "eog", "xdg-open", "display"):
            import shutil
            if shutil.which(v):
                viewer = v
                break
        if viewer:
            subprocess.Popen([viewer, out_file])
            print(f"[OK] Відкрито через {viewer}")
    except Exception:
        pass


if __name__ == "__main__":
    main()
