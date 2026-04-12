#!/usr/bin/env python3
"""
navmesh_3d.py — 3D-рендер Detour navmesh (.bin) у стилі RecastDemo.

Парсить: NMSH header → tile data → dtMeshHeader → vertices → polygons →
         detail mesh → detail vertices → detail triangles → Poly3DCollection.

Використання:
  python3 scripts/navmesh_3d.py [navmesh.bin] [output.png]
  python3 scripts/navmesh_3d.py navmesh_new.bin navmesh_3d_preview.png
"""

import struct
import sys
import os
import math


# ─── Detour struct sizes (від DetourNavMeshBuilder.cpp) ────────────────────────

def align4(n):
    return (n + 3) & ~3

# dtMeshHeader: 15 int/uint + 3 float + float[3] + float[3] + float = 100 bytes
HEADER_FMT  = '<iiiiIiiiiiiiiiifff3f3ff'
HEADER_SIZE = struct.calcsize(HEADER_FMT)  # 100

# dtPoly: uint firstLink + ushort[6] verts + ushort[6] neis + ushort flags + uchar×2
POLY_FMT    = '<I6H6HHBBxx'          # +2 pad → 36? Let's compute:
# I=4, 6H=12, 6H=12, H=2, B=1, B=1, 2 pad=2 → 34? No, struct pads to 4:
# actual struct in C: 4+12+12+2+1+1 = 32, already divisible by 4, no padding needed
POLY_FMT    = '<I6H6HHBBxx'          # xx = 2 explicit pads, total=36
# Перевіримо
POLY_SIZE   = struct.calcsize(POLY_FMT)   # should be 36
# Hmm, C compiler may not pad since 32 is already aligned. Let's detect at runtime.

# dtPolyDetail: uint×2 + uchar×2 → 10 bytes, dtAlign4 → 12
DETAIL_MESH_FMT  = '<IIBBxx'
DETAIL_MESH_SIZE = struct.calcsize(DETAIL_MESH_FMT)  # 12

# dtLink: uint×2 + uchar×4 = 12
LINK_SIZE = 12

# dtBVNode: ushort[3] + ushort[3] + int = 16
BV_NODE_SIZE = 16


def detect_poly_size(tile_data, header):
    """
    Детектує реальний розмір dtPoly:
    спробуємо 32 і 36, перевіряємо чи vertCount перших полігонів виглядає розумно.
    """
    hdr_sz  = align4(HEADER_SIZE)
    vert_sz = align4(header['vertCount'] * 12)
    off = hdr_sz + vert_sz
    for sz in (32, 36):
        ok = True
        for i in range(min(header['polyCount'], 10)):
            p_off = off + i * sz
            if p_off + sz > len(tile_data):
                ok = False; break
            vc = tile_data[p_off + sz - 2]   # uchar vertCount є передостаннім (без pad)
            if sz == 36:
                vc = struct.unpack_from('<B', tile_data, p_off + 32)[0]
            else:
                vc = struct.unpack_from('<B', tile_data, p_off + 30)[0]
            if not (1 <= vc <= 6):
                ok = False; break
        if ok:
            return sz
    return 32


def parse_nmsh(path):
    with open(path, 'rb') as f:
        data = f.read()

    magic, = struct.unpack_from('<I', data, 0)
    if magic != 0x4E4D5348:
        raise ValueError(f"[ERR] Bad magic: {hex(magic)} (очікувалось 0x4E4D5348 'NMSH')")

    size, = struct.unpack_from('<I', data, 4)
    if len(data) < 8 + size:
        raise ValueError(f"[ERR] Файл обрізаний: {len(data)} < {8+size}")

    tile_data = data[8:8 + size]
    return parse_tile(tile_data)


def parse_tile(tile_data):
    # ── Header ────────────────────────────────────────────────────────────────
    raw = struct.unpack_from(HEADER_FMT, tile_data, 0)
    H = {
        'magic':          raw[0],
        'version':        raw[1],
        'x':              raw[2],  'y': raw[3], 'layer': raw[4],
        'userId':         raw[5],
        'polyCount':      raw[6],
        'vertCount':      raw[7],
        'maxLinkCount':   raw[8],
        'detailMeshCount':raw[9],
        'detailVertCount':raw[10],
        'detailTriCount': raw[11],
        'bvNodeCount':    raw[12],
        'offMeshConCount':raw[13],
        'offMeshBase':    raw[14],
        'walkableHeight': raw[15],
        'walkableRadius': raw[16],
        'walkableClimb':  raw[17],
        'bmin':           raw[18:21],
        'bmax':           raw[21:24],
        'bvQuantFactor':  raw[24],
    }

    MAGIC_EXPECTED = (ord('D') << 24) | (ord('N') << 16) | (ord('A') << 8) | ord('V')
    if H['magic'] != MAGIC_EXPECTED:
        raise ValueError(f"[ERR] Невірний Detour magic у tile: {hex(H['magic'])}")

    hdr_sz  = align4(HEADER_SIZE)

    # ── Vertices ───────────────────────────────────────────────────────────────
    vc = H['vertCount']
    vert_flat = struct.unpack_from(f'<{vc*3}f', tile_data, hdr_sz)
    verts = [(vert_flat[i*3], vert_flat[i*3+1], vert_flat[i*3+2]) for i in range(vc)]
    vert_sz = align4(vc * 12)

    # ── Polygons ───────────────────────────────────────────────────────────────
    poly_sz = detect_poly_size(tile_data, H)
    pc = H['polyCount']
    polys_off = hdr_sz + vert_sz
    polys = []
    for i in range(pc):
        off = polys_off + i * poly_sz
        raw = tile_data[off:off + poly_sz]
        first_link = struct.unpack_from('<I', raw, 0)[0]
        pverts     = struct.unpack_from('<6H', raw, 4)
        neis       = struct.unpack_from('<6H', raw, 16)
        flags      = struct.unpack_from('<H', raw, 28)[0]
        v_count    = raw[30]
        area_type  = raw[31]
        polys.append({
            'firstLink': first_link,
            'verts':     list(pverts),
            'neis':      list(neis),
            'flags':     flags,
            'vertCount': v_count,
            'area':      area_type & 0x3F,
            'type':      (area_type >> 6) & 0x3,
        })

    polys_sz  = align4(pc * poly_sz)
    links_sz  = align4(H['maxLinkCount'] * LINK_SIZE)

    # ── Detail meshes ──────────────────────────────────────────────────────────
    dm_off_start = polys_off + polys_sz + links_sz
    dmc = H['detailMeshCount']
    detail_meshes = []
    for i in range(dmc):
        off = dm_off_start + i * DETAIL_MESH_SIZE
        dm = struct.unpack_from('<IIBBxx', tile_data, off)
        detail_meshes.append({'vertBase': dm[0], 'triBase': dm[1],
                              'vertCount': dm[2], 'triCount': dm[3]})
    dm_sz = align4(dmc * DETAIL_MESH_SIZE)

    # ── Detail vertices ────────────────────────────────────────────────────────
    dv_off = dm_off_start + dm_sz
    dvc = H['detailVertCount']
    if dvc > 0:
        dv_flat = struct.unpack_from(f'<{dvc*3}f', tile_data, dv_off)
        detail_verts = [(dv_flat[i*3], dv_flat[i*3+1], dv_flat[i*3+2]) for i in range(dvc)]
    else:
        detail_verts = []
    dv_sz = align4(dvc * 12)

    # ── Detail triangles ───────────────────────────────────────────────────────
    dt_off = dv_off + dv_sz
    dtc = H['detailTriCount']
    if dtc > 0:
        dt_flat = struct.unpack_from(f'<{dtc*4}B', tile_data, dt_off)
        detail_tris = [(dt_flat[i*4], dt_flat[i*4+1], dt_flat[i*4+2], dt_flat[i*4+3])
                       for i in range(dtc)]
    else:
        detail_tris = []

    return H, verts, polys, detail_meshes, detail_verts, detail_tris


def build_triangles(H, verts, polys, detail_meshes, detail_verts, detail_tris):
    """
    Будує список трикутників для рендерингу.
    Повертає: list of (tri_pts_3d, area_type)
    """
    triangles = []
    n_poly = min(H['detailMeshCount'], H['polyCount'])

    for pi in range(n_poly):
        poly = polys[pi]
        dm   = detail_meshes[pi]
        pvc  = poly['vertCount']

        for ti in range(dm['triCount']):
            t = detail_tris[dm['triBase'] + ti]
            pts = []
            for k in range(3):
                vi = t[k]
                if vi < pvc:
                    v = verts[poly['verts'][vi]]
                else:
                    idx = dm['vertBase'] + (vi - pvc)
                    if idx < len(detail_verts):
                        v = detail_verts[idx]
                    else:
                        v = verts[poly['verts'][0]]
                pts.append(v)
            triangles.append((pts, poly['area'], poly['type']))

    # Якщо detail mesh порожній — fallback: fan triangulation базових полігонів
    if not triangles:
        for poly in polys:
            pvc = poly['vertCount']
            if pvc < 3: continue
            v0 = verts[poly['verts'][0]]
            for i in range(1, pvc - 1):
                v1 = verts[poly['verts'][i]]
                v2 = verts[poly['verts'][i + 1]]
                triangles.append(([v0, v1, v2], poly['area'], poly['type']))

    return triangles


def area_color(area, tri_type, z, z_min, z_max):
    """
    Кольори як у RecastDemo:
    type 0 = ground/walkable → зелений градієнт по висоті
    type 2 = offmesh → помаранчевий
    area 0 = не ходиться → сірий
    """
    if tri_type == 2:  # DT_POLYTYPE_OFFMESH_CONNECTION
        return (1.0, 0.5, 0.1, 0.9)
    if area == 0:      # RC_NULL_AREA (не ходиться, але в mesh — зазвичай не буває)
        return (0.3, 0.3, 0.3, 0.6)

    # Висотний градієнт: нижче → синьо-зелений, вище → жовто-зелений
    t = (z - z_min) / (z_max - z_min + 1e-6)
    t = max(0.0, min(1.0, t))
    # від (0.1, 0.6, 0.3) до (0.6, 0.9, 0.2)  (RecastDemo-style green)
    r = 0.1 + t * 0.5
    g = 0.6 + t * 0.3
    b = 0.3 - t * 0.1
    return (r, g, b, 0.82)


def main():
    bin_file = sys.argv[1] if len(sys.argv) > 1 else "navmesh_new.bin"
    out_file = sys.argv[2] if len(sys.argv) > 2 else "navmesh_3d_preview.png"

    if not os.path.exists(bin_file):
        print(f"[ERR] Файл не знайдено: {bin_file}")
        sys.exit(1)

    print(f"[..] Парсимо {bin_file}...")
    try:
        H, verts, polys, detail_meshes, detail_verts, detail_tris = parse_nmsh(bin_file)
    except Exception as e:
        print(e)
        sys.exit(1)

    print(f"[OK] Tile: polys={H['polyCount']} verts={H['vertCount']}")
    print(f"     Detail: meshes={H['detailMeshCount']} verts={H['detailVertCount']} tris={H['detailTriCount']}")
    print(f"     AABB: ({H['bmin'][0]:.0f},{H['bmin'][1]:.0f},{H['bmin'][2]:.0f}) → "
          f"({H['bmax'][0]:.0f},{H['bmax'][1]:.0f},{H['bmax'][2]:.0f})")

    tris = build_triangles(H, verts, polys, detail_meshes, detail_verts, detail_tris)
    print(f"[OK] Трикутників для рендеру: {len(tris)}")

    if not tris:
        print("[ERR] Немає трикутників — пустий mesh?")
        sys.exit(1)

    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        from mpl_toolkits.mplot3d.art3d import Poly3DCollection
        from mpl_toolkits.mplot3d import Axes3D
    except ImportError:
        print("[ERR] pip install matplotlib")
        sys.exit(1)

    # Координатне відображення (build_navmesh.cpp):
    #   L2 X → Recast X (axis 0)
    #   L2 Z (висота) → Recast Y (axis 1)  ← завжди висота в нашому build_navmesh
    #   L2 Y (північ)  → Recast Z (axis 2)
    bmin, bmax = H['bmin'], H['bmax']
    dy = bmax[1] - bmin[1]
    dz = bmax[2] - bmin[2]
    height_axis = 1   # Recast Y = L2 height (завжди для нашого build_navmesh.cpp)
    print(f"[OK] Координати: X=L2_X  Y(up)=L2_Z  Z=L2_Y  "
          f"(height_range={dy:.0f}  horiz={dz:.0f} L2u)")

    z_vals = [v[height_axis] for tri in tris for v in tri[0]]
    z_min, z_max = min(z_vals), max(z_vals)

    # ── Збираємо Poly3DCollection ─────────────────────────────────────────────
    poly_verts = []
    face_colors = []
    edge_colors = []

    for pts, area, tri_type in tris:
        # Recast vertex = (L2_X, L2_Z_height, L2_Y_north)
        # → mpl 3D: x=L2_X, y=L2_Y (north), z=L2_Z (height=up)
        tri3d = [(p[0], p[2], p[1]) for p in pts]
        z_avg = sum(p[1] for p in pts) / 3   # Recast Y = height

        poly_verts.append(tri3d)
        fc = area_color(area, tri_type, z_avg, z_min, z_max)
        face_colors.append(fc)
        # Wireframe колір: трохи темніший
        edge_colors.append((fc[0]*0.4, fc[1]*0.4, fc[2]*0.4, 0.5))

    # ── Рендер ────────────────────────────────────────────────────────────────
    fig = plt.figure(figsize=(14, 10), facecolor="#1a1a2e")
    ax  = fig.add_subplot(111, projection='3d')
    ax.set_facecolor("#0d0d1a")

    collection = Poly3DCollection(poly_verts, zsort='average')
    collection.set_facecolors(face_colors)
    collection.set_edgecolors(edge_colors)
    collection.set_linewidth(0.3)
    ax.add_collection3d(collection)

    # Авто-масштаб
    # poly_verts already swapped: (L2_X, L2_Y_north, L2_Z_height)
    all_pts_swapped = [p for tri in poly_verts for p in tri]
    xs = [p[0] for p in all_pts_swapped]
    ys = [p[1] for p in all_pts_swapped]
    zs = [p[2] for p in all_pts_swapped]

    ax.set_xlim(min(xs), max(xs))
    ax.set_ylim(min(ys), max(ys))
    ax.set_zlim(min(zs), max(zs))

    # Кут огляду
    ax.view_init(elev=35, azim=-60)

    ax.set_xlabel('L2_X (East)', color='#8888aa', fontsize=8)
    ax.set_ylabel('L2_Y (North)', color='#8888aa', fontsize=8)
    ax.set_zlabel('L2_Z (Height)', color='#8888aa', fontsize=8)
    ax.tick_params(colors='#666688', labelsize=6)
    ax.xaxis.pane.fill = False
    ax.yaxis.pane.fill = False
    ax.zaxis.pane.fill = False
    ax.xaxis.pane.set_edgecolor('#222244')
    ax.yaxis.pane.set_edgecolor('#222244')
    ax.zaxis.pane.set_edgecolor('#222244')
    ax.grid(True, color='#222244', linewidth=0.4, alpha=0.5)

    tile_w = bmax[0] - bmin[0]
    tile_h = bmax[1] - bmin[1]
    fig.suptitle(
        f"rdga1bot NavMesh 3D  |  {bin_file}\n"
        f"{H['polyCount']} polys  {len(tris)} tris  {H['detailVertCount']} detail verts  "
        f"|  {tile_w:.0f}×{tile_h:.0f} L2u",
        color='#d0d0ff', fontsize=10
    )

    plt.tight_layout()
    plt.savefig(out_file, dpi=150, bbox_inches='tight', facecolor=fig.get_facecolor())
    plt.close()
    print(f"[OK] Збережено: {out_file}")

    try:
        import subprocess, shutil
        for v in ('feh', 'eog', 'xdg-open'):
            if shutil.which(v):
                subprocess.Popen([v, out_file])
                break
    except Exception:
        pass


if __name__ == '__main__':
    main()
