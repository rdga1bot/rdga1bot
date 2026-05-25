// bsp_to_navmesh: extracts BSP floor geometry from .unr tiles → Detour navmesh .bin
//
// Usage:
//   bsp_to_navmesh <L2root> <output.bin> [map1 map2 ...]
//   e.g.: bsp_to_navmesh /home/rdga1/deiceland navmesh_toi11.bin 17_21 17_22 18_21 18_22
//
// Coord mapping (UE2 → Recast → L2):
//   UE x,y,z → Recast x=UE.x, y=UE.z(up), z=UE.y → L2 X=Recast.x, Z=Recast.y, Y=Recast.z

#include <unreal/PackageLoader.h>
#include <unreal/Level.h>
#include <unreal/BSP.h>

#include "Recast.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshBuilder.h"
#include "DetourCommon.h"

#include <iostream>
#include <fstream>
#include <vector>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <string>

// ── BSP extraction ─────────────────────────────────────────────────────────────

struct Tri3 { float v[9]; }; // 3 vertices × (x,y,z)

static void extract_bsp_floor_tris(const unreal::Model& m,
                                   std::vector<float>& verts,
                                   std::vector<int>& tris)
{
    auto& pts  = m.points;
    auto& nds  = m.nodes;
    auto& vrts = m.vertices;
    auto& srfs = m.surfaces;

    for (const auto& node : nds) {
        if (node.vertex_count < 3) continue;

        // NF_NotCsg (0x01) = passable; skip
        if (node.flags & unreal::NF_NotCsg) continue;

        // Surface flags
        int si = node.surface_index.value;
        if (si < 0 || static_cast<size_t>(si) >= srfs.size()) continue;
        const auto& surf = srfs[si];
        if (surf.polygon_flags & unreal::PF_NotSolid)  continue;
        if (surf.polygon_flags & unreal::PF_Invisible) continue;

        // Only upward-facing (floor) surfaces: plane.z > 0.5 means normal ≈ up
        // UE Z = up → Recast Y; walkable if slope < 50°
        if (node.plane.z < 0.5f) continue;

        // Collect polygon vertices
        int vpi = node.vertex_pool_index.value;
        int vc  = node.vertex_count;
        if (vpi < 0) continue;

        // Fan triangulation (convex polygon → N-2 triangles)
        // Gather first vertex index
        // Filter threshold: UE2 "huge brush" zone clips sit at ±327680.
        // Real L2 game geometry stays well within ±250000.
        static constexpr float MAX_COORD = 250000.f;
        auto get_pt = [&](int i) -> const unreal::Vector* {
            int vi = vpi + i;
            if (vi < 0 || static_cast<size_t>(vi) >= vrts.size()) return nullptr;
            int pi = vrts[vi].vertex_index.value;
            if (pi < 0 || static_cast<size_t>(pi) >= pts.size()) return nullptr;
            const auto& p = pts[pi];
            if (std::fabs(p.x) >= MAX_COORD || std::fabs(p.y) >= MAX_COORD ||
                std::fabs(p.z) >= MAX_COORD)
                return nullptr;
            return &p;
        };

        const unreal::Vector* p0 = get_pt(0);
        if (!p0) continue;

        for (int i = 1; i + 1 < vc; ++i) {
            const unreal::Vector* p1 = get_pt(i);
            const unreal::Vector* p2 = get_pt(i + 1);
            if (!p1 || !p2) continue;

            int base = static_cast<int>(verts.size() / 3);
            // UE (x,y,z) → Recast (x, z_up=UE.z, z_north=UE.y)
            verts.push_back(p0->x); verts.push_back(p0->z); verts.push_back(p0->y);
            verts.push_back(p1->x); verts.push_back(p1->z); verts.push_back(p1->y);
            verts.push_back(p2->x); verts.push_back(p2->z); verts.push_back(p2->y);
            tris.push_back(base);
            tris.push_back(base + 1);
            tris.push_back(base + 2);
        }
    }
}

// ── Recast pipeline ─────────────────────────────────────────────────────────────

static bool build_navmesh(const std::vector<float>& verts,
                          const std::vector<int>&   tris,
                          const char*               output_path)
{
    if (tris.empty()) {
        std::cerr << "[BUILD] No triangles to build navmesh from.\n";
        return false;
    }

    std::cerr << "[BUILD] Triangles: " << tris.size()/3
              << "  Vertices: " << verts.size()/3 << "\n";

    // L2 units: char ~180 L2u tall, ~40 L2u wide
    rcContext ctx(false);
    rcConfig cfg{};
    memset(&cfg, 0, sizeof(cfg));

    cfg.cs                   = 40.f;
    cfg.ch                   = 20.f;
    cfg.walkableSlopeAngle   = 50.f;
    cfg.walkableHeight       = static_cast<int>(std::ceil(180.f / cfg.ch));  // 9
    cfg.walkableClimb        = static_cast<int>(std::floor(30.f  / cfg.ch)); // 1
    cfg.walkableRadius       = static_cast<int>(std::ceil(30.f   / cfg.cs)); // 1
    cfg.maxEdgeLen           = static_cast<int>(1200.f / cfg.cs);
    cfg.maxSimplificationError = 1.3f;
    cfg.minRegionArea        = 2;
    cfg.mergeRegionArea      = 10;
    cfg.maxVertsPerPoly      = 6;
    cfg.detailSampleDist     = cfg.cs;
    cfg.detailSampleMaxError = cfg.ch * 0.5f;

    rcCalcBounds(verts.data(), static_cast<int>(verts.size()/3), cfg.bmin, cfg.bmax);
    const float xz_pad = (cfg.walkableRadius + 1) * cfg.cs;
    cfg.bmin[0] -= xz_pad;  cfg.bmax[0] += xz_pad;
    cfg.bmin[2] -= xz_pad;  cfg.bmax[2] += xz_pad;
    cfg.bmin[1] -= cfg.ch;
    cfg.bmax[1] += (cfg.walkableHeight + 1) * cfg.ch;
    rcCalcGridSize(cfg.bmin, cfg.bmax, cfg.cs, &cfg.width, &cfg.height);
    std::cerr << "[BUILD] Grid: " << cfg.width << "×" << cfg.height << "\n";

    rcHeightfield* hf = rcAllocHeightfield();
    if (!rcCreateHeightfield(&ctx, *hf, cfg.width, cfg.height,
                              cfg.bmin, cfg.bmax, cfg.cs, cfg.ch)) {
        std::cerr << "[BUILD] rcCreateHeightfield failed\n"; return false;
    }

    std::vector<uint8_t> areas(tris.size()/3, RC_WALKABLE_AREA);
    if (!rcRasterizeTriangles(&ctx, verts.data(), static_cast<int>(verts.size()/3),
                               tris.data(), areas.data(),
                               static_cast<int>(tris.size()/3),
                               *hf, cfg.walkableClimb)) {
        std::cerr << "[BUILD] rcRasterizeTriangles failed\n"; return false;
    }

    rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *hf);
    // rcFilterLedgeSpans intentionally skipped (L2 dungeon geometry has
    // isolated floor sections with no neighbours → filter removes all spans)
    rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *hf);

    rcCompactHeightfield* chf = rcAllocCompactHeightfield();
    if (!rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb,
                                    *hf, *chf)) {
        std::cerr << "[BUILD] rcBuildCompactHeightfield failed\n"; return false;
    }
    rcFreeHeightField(hf);

    if (!rcErodeWalkableArea(&ctx, cfg.walkableRadius, *chf)) {
        std::cerr << "[BUILD] rcErodeWalkableArea failed\n"; return false;
    }
    if (!rcBuildDistanceField(&ctx, *chf)) {
        std::cerr << "[BUILD] rcBuildDistanceField failed\n"; return false;
    }
    if (!rcBuildRegions(&ctx, *chf, 0, cfg.minRegionArea, cfg.mergeRegionArea)) {
        std::cerr << "[BUILD] rcBuildRegions failed\n"; return false;
    }

    rcContourSet* cset = rcAllocContourSet();
    if (!rcBuildContours(&ctx, *chf, cfg.maxSimplificationError,
                          cfg.maxEdgeLen, *cset)) {
        std::cerr << "[BUILD] rcBuildContours failed\n"; return false;
    }
    rcPolyMesh* pmesh = rcAllocPolyMesh();
    if (!rcBuildPolyMesh(&ctx, *cset, cfg.maxVertsPerPoly, *pmesh)) {
        std::cerr << "[BUILD] rcBuildPolyMesh failed\n"; return false;
    }
    rcPolyMeshDetail* dmesh = rcAllocPolyMeshDetail();
    if (!rcBuildPolyMeshDetail(&ctx, *pmesh, *chf,
                                cfg.detailSampleDist,
                                cfg.detailSampleMaxError, *dmesh)) {
        std::cerr << "[BUILD] rcBuildPolyMeshDetail failed\n"; return false;
    }
    rcFreeCompactHeightfield(chf);
    rcFreeContourSet(cset);
    std::cerr << "[BUILD] Polys: " << pmesh->npolys << "\n";

    for (int i = 0; i < pmesh->npolys; ++i)
        if (pmesh->areas[i] == RC_WALKABLE_AREA)
            pmesh->flags[i] = 1;

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
        std::cerr << "[BUILD] dtCreateNavMeshData failed\n"; return false;
    }
    std::cerr << "[BUILD] NavMesh data: " << nav_size << " bytes\n";

    rcFreePolyMesh(pmesh);
    rcFreePolyMeshDetail(dmesh);

    std::ofstream out(output_path, std::ios::binary);
    if (!out) {
        std::cerr << "[BUILD] Cannot write: " << output_path << "\n";
        dtFree(nav_data); return false;
    }
    const uint32_t magic = 0x4E4D5348; // "NMSH"
    const uint32_t sz    = static_cast<uint32_t>(nav_size);
    out.write(reinterpret_cast<const char*>(&magic), 4);
    out.write(reinterpret_cast<const char*>(&sz),    4);
    out.write(reinterpret_cast<const char*>(nav_data), nav_size);
    dtFree(nav_data);

    std::cerr << "[BUILD] Saved: " << output_path << "\n";
    return true;
}

// ── main ───────────────────────────────────────────────────────────────────────

int main(int argc, char** argv)
{
    if (argc < 4) {
        std::cerr << "Usage: bsp_to_navmesh <L2root> <output.bin> <map1> [map2 ...]\n";
        std::cerr << "  e.g.: bsp_to_navmesh /home/rdga1/deiceland navmesh_toi11.bin 17_21 17_22 18_21 18_22\n";
        return 1;
    }

    const std::string l2root    = argv[1];
    const std::string out_path  = argv[2];

    std::vector<unreal::SearchConfig> configs;
    configs.push_back(unreal::SearchConfig{"MAPS",         "unr"});
    configs.push_back(unreal::SearchConfig{"StaticMeshes", "usx"});
    configs.push_back(unreal::SearchConfig{"Textures",     "utx"});
    configs.push_back(unreal::SearchConfig{"SysTextures",  "utx"});
    unreal::PackageLoader loader{l2root, configs};

    std::vector<float> all_verts;
    std::vector<int>   all_tris;

    for (int i = 3; i < argc; ++i) {
        std::string map_name = argv[i];
        std::cerr << "[LOAD] " << map_name << "\n";

        auto opt = loader.load_package(map_name);
        if (!opt) { std::cerr << "  Failed to load\n"; continue; }
        auto& pkg = *opt;

        std::vector<std::shared_ptr<unreal::Level>> levels;
        pkg.load_objects("Level", levels);
        if (levels.empty()) { std::cerr << "  No Level object\n"; continue; }

        // Access model via ObjectRef — triggers deserialization
        std::shared_ptr<unreal::Model> model = levels[0]->model;
        if (!model) { std::cerr << "  No Model\n"; continue; }

        size_t before = all_tris.size();
        extract_bsp_floor_tris(*model, all_verts, all_tris);
        std::cerr << "  Floor tris: " << (all_tris.size() - before)/3 << "\n";
    }

    if (all_tris.empty()) {
        std::cerr << "[BUILD] No floor geometry found.\n";
        return 1;
    }

    return build_navmesh(all_verts, all_tris, out_path.c_str()) ? 0 : 1;
}
