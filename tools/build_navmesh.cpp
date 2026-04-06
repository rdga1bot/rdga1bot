// Офлайн build: читає .pts (зібрані ботом) → будує NavMesh через Recast →
// зберігає .bin (Detour binary tile data).
//
// Компіляція:
//   g++ -std=c++17 -O2 tools/build_navmesh.cpp \
//       src/recast/Recast/Source/*.cpp \
//       src/recast/Detour/Source/*.cpp \
//       -Isrc/recast/Recast/Include \
//       -Isrc/recast/Detour/Include \
//       -o tools/build_navmesh
//
// Клонування Recast (одноразово):
//   cd src && mkdir -p recast && cd recast
//   git clone --depth 1 https://github.com/recastnavigation/recastnavigation.git tmp
//   cp -r tmp/Recast . && cp -r tmp/Detour . && rm -rf tmp
//
// Використання:
//   ./tools/build_navmesh navmesh_points.pts navmesh.bin

#include <iostream>
#include <fstream>
#include <vector>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <algorithm>

// Recast/Detour headers
#include "Recast.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshBuilder.h"
#include "DetourNavMeshQuery.h"
#include "DetourCommon.h"

struct NavPoint { float x, y, z; };

// Читаємо .pts файл
std::vector<NavPoint> loadPoints(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { std::cerr << "Cannot open: " << path << "\n"; return {}; }
    uint32_t n = 0;
    f.read((char*)&n, 4);
    std::vector<NavPoint> pts(n);
    f.read((char*)pts.data(), n * 12);
    std::cerr << "[BUILD] Завантажено " << n << " точок\n";
    return pts;
}

// Генеруємо triangle mesh: навколо кожної точки — плоский quad 160×160 L2u
// (2 трикутники). Recast об'єднає накладені quad-и в зв'язну поверхню.
void buildMesh(const std::vector<NavPoint>& pts,
               std::vector<float>& verts,
               std::vector<int>& tris,
               float quad_half = 80.f) {
    verts.reserve(pts.size() * 12);
    tris.reserve(pts.size() * 6);

    for (const auto& p : pts) {
        // 4 вершини quad: Recast Y=вгору, ми кладемо Z=0 (flat 2D)
        // L2: X=East, Y=North → Recast: x=X, y=Z(висота), z=Y
        int b = (int)verts.size() / 3;

        verts.push_back(p.x - quad_half); verts.push_back(p.z); verts.push_back(p.y - quad_half);
        verts.push_back(p.x + quad_half); verts.push_back(p.z); verts.push_back(p.y - quad_half);
        verts.push_back(p.x + quad_half); verts.push_back(p.z); verts.push_back(p.y + quad_half);
        verts.push_back(p.x - quad_half); verts.push_back(p.z); verts.push_back(p.y + quad_half);

        tris.push_back(b+0); tris.push_back(b+1); tris.push_back(b+2);
        tris.push_back(b+0); tris.push_back(b+2); tris.push_back(b+3);
    }
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: build_navmesh <points.pts> <output.bin>\n";
        return 1;
    }

    auto pts = loadPoints(argv[1]);
    if (pts.empty()) return 1;

    // ── Генеруємо geometry ────────────────────────────────────────────────────
    std::vector<float> verts;
    std::vector<int>   tris;
    buildMesh(pts, verts, tris);
    std::cerr << "[BUILD] Vertices: " << verts.size()/3
              << " Tris: " << tris.size()/3 << "\n";

    // ── Recast config (L2 одиниці, не метри) ─────────────────────────────────
    // L2u: персонаж ~180 L2u висота, ширина ~40 L2u
    rcContext ctx(false);
    rcConfig cfg{};
    memset(&cfg, 0, sizeof(cfg));

    cfg.cs              = 40.f;    // cell size = 40 L2u (ширина персонажа)
    cfg.ch              = 20.f;    // cell height = 20 L2u
    cfg.walkableSlopeAngle  = 50.f;
    cfg.walkableHeight      = (int)std::ceil(180.f / cfg.ch); // 9 cells
    cfg.walkableClimb       = (int)std::floor(30.f  / cfg.ch); // 1 cell
    cfg.walkableRadius      = (int)std::ceil(30.f   / cfg.cs); // 1 cell
    cfg.maxEdgeLen          = (int)(1200.f / cfg.cs);
    cfg.maxSimplificationError = 1.3f;
    cfg.minRegionArea       = 2;   // тримаємо малі регіони (2 клітини мінімум)
    cfg.mergeRegionArea     = 10;
    cfg.maxVertsPerPoly     = 6;
    // detailSampleDist=0 → мінімальна тріангуляція → Recast triangulateHull segfault
    // з маленькими полігонами. Використовуємо 1 cell = cs.
    cfg.detailSampleDist    = cfg.cs;
    cfg.detailSampleMaxError= cfg.ch * 0.5f;

    // AABB з точок
    rcCalcBounds(verts.data(), (int)verts.size()/3, cfg.bmin, cfg.bmax);
    // Padding: XZ — для walkableRadius (агент по горизонталі)
    //          Y-мін — 1 cell вниз (small margin)
    //          Y-макс — walkableHeight cells ВГОРУ (потрібно місце для агента над поверхнею!)
    //   Без достатнього Y-max padding: clearance над спаном < walkableHeight →
    //   rcBuildCompactHeightfield відфільтровує ВСІ спани → Polys=0.
    const float xz_pad = (cfg.walkableRadius + 1) * cfg.cs;
    cfg.bmin[0] -= xz_pad;   cfg.bmax[0] += xz_pad;
    cfg.bmin[2] -= xz_pad;   cfg.bmax[2] += xz_pad;
    cfg.bmin[1] -= cfg.ch;                                    // 1 cell вниз
    cfg.bmax[1] += (cfg.walkableHeight + 1) * cfg.ch;        // ~200 L2u вгору
    rcCalcGridSize(cfg.bmin, cfg.bmax, cfg.cs, &cfg.width, &cfg.height);
    std::cerr << "[BUILD] Grid: " << cfg.width << "×" << cfg.height << "\n";

    // ── Heightfield → CompactHeightfield ─────────────────────────────────────
    rcHeightfield* hf = rcAllocHeightfield();
    if (!rcCreateHeightfield(&ctx, *hf, cfg.width, cfg.height,
                              cfg.bmin, cfg.bmax, cfg.cs, cfg.ch)) {
        std::cerr << "[BUILD] rcCreateHeightfield failed\n"; return 1;
    }

    std::vector<uint8_t> areas(tris.size()/3, RC_WALKABLE_AREA);
    if (!rcRasterizeTriangles(&ctx, verts.data(), (int)verts.size()/3,
                               tris.data(), areas.data(), (int)tris.size()/3,
                               *hf, cfg.walkableClimb)) {
        std::cerr << "[BUILD] rcRasterizeTriangles failed\n"; return 1;
    }

    rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *hf);
    // rcFilterLedgeSpans: ВИМКНЕНО — для підземелля L2 з ізольованими flat квадами
    // цей фільтр видаляє ВСІ граничні клітини (сусід = порожньо = drop 21 cells > walkableHeight=9)
    // → лишається тільки 1 inner cell → Contours=0 → Polys=0.
    // rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *hf);
    rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *hf);

    rcCompactHeightfield* chf = rcAllocCompactHeightfield();
    if (!rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb,
                                    *hf, *chf)) {
        std::cerr << "[BUILD] rcBuildCompactHeightfield failed\n"; return 1;
    }
    rcFreeHeightField(hf);

    if (!rcErodeWalkableArea(&ctx, cfg.walkableRadius, *chf)) {
        std::cerr << "[BUILD] rcErodeWalkableArea failed\n"; return 1;
    }

    if (!rcBuildDistanceField(&ctx, *chf)) {
        std::cerr << "[BUILD] rcBuildDistanceField failed\n"; return 1;
    }

    if (!rcBuildRegions(&ctx, *chf, 0, cfg.minRegionArea, cfg.mergeRegionArea)) {
        std::cerr << "[BUILD] rcBuildRegions failed\n"; return 1;
    }

    // ── ContourSet → PolyMesh ────────────────────────────────────────────────
    rcContourSet* cset = rcAllocContourSet();
    if (!rcBuildContours(&ctx, *chf, cfg.maxSimplificationError,
                          cfg.maxEdgeLen, *cset)) {
        std::cerr << "[BUILD] rcBuildContours failed\n"; return 1;
    }
    rcPolyMesh* pmesh = rcAllocPolyMesh();
    if (!rcBuildPolyMesh(&ctx, *cset, cfg.maxVertsPerPoly, *pmesh)) {
        std::cerr << "[BUILD] rcBuildPolyMesh failed\n"; return 1;
    }

    rcPolyMeshDetail* dmesh = rcAllocPolyMeshDetail();
    if (!rcBuildPolyMeshDetail(&ctx, *pmesh, *chf,
                                cfg.detailSampleDist,
                                cfg.detailSampleMaxError, *dmesh)) {
        std::cerr << "[BUILD] rcBuildPolyMeshDetail failed\n"; return 1;
    }

    rcFreeCompactHeightfield(chf);
    rcFreeContourSet(cset);
    std::cerr << "[BUILD] Polys: " << pmesh->npolys << "\n";

    // ── Детур: встановлюємо flags ─────────────────────────────────────────────
    for (int i = 0; i < pmesh->npolys; i++)
        if (pmesh->areas[i] == RC_WALKABLE_AREA)
            pmesh->flags[i] = 1;

    // ── dtCreateNavMeshData ───────────────────────────────────────────────────
    dtNavMeshCreateParams params{};
    memset(&params, 0, sizeof(params));
    params.verts            = pmesh->verts;
    params.vertCount        = pmesh->nverts;
    params.polys            = pmesh->polys;
    params.polyAreas        = pmesh->areas;
    params.polyFlags        = pmesh->flags;
    params.polyCount        = pmesh->npolys;
    params.nvp              = pmesh->nvp;
    params.detailMeshes     = dmesh->meshes;
    params.detailVerts      = dmesh->verts;
    params.detailVertsCount = dmesh->nverts;
    params.detailTris       = dmesh->tris;
    params.detailTriCount   = dmesh->ntris;
    params.walkableHeight   = cfg.walkableHeight * cfg.ch;
    params.walkableRadius   = cfg.walkableRadius * cfg.cs;
    params.walkableClimb    = cfg.walkableClimb  * cfg.ch;
    rcVcopy(params.bmin, pmesh->bmin);
    rcVcopy(params.bmax, pmesh->bmax);
    params.cs        = cfg.cs;
    params.ch        = cfg.ch;
    params.buildBvTree = true;

    uint8_t* nav_data = nullptr;
    int      nav_size = 0;
    if (!dtCreateNavMeshData(&params, &nav_data, &nav_size)) {
        std::cerr << "[BUILD] dtCreateNavMeshData failed\n"; return 1;
    }
    std::cerr << "[BUILD] NavMesh data: " << nav_size << " bytes\n";

    rcFreePolyMesh(pmesh);
    rcFreePolyMeshDetail(dmesh);

    // ── Зберігаємо .bin: uint32_t magic + uint32_t size + raw tile data ───────
    // Формат: uint32_t magic=0x4E4D5348 ("NMSH"), uint32_t size, <data>
    std::ofstream out(argv[2], std::ios::binary);
    if (!out) { std::cerr << "[BUILD] Cannot write: " << argv[2] << "\n";
                dtFree(nav_data); return 1; }

    uint32_t magic = 0x4E4D5348;  // "NMSH"
    uint32_t size  = (uint32_t)nav_size;
    out.write((const char*)&magic, 4);
    out.write((const char*)&size,  4);
    out.write((const char*)nav_data, nav_size);
    dtFree(nav_data);

    std::cerr << "[BUILD] Збережено: " << argv[2] << "\n";
    return 0;
}
