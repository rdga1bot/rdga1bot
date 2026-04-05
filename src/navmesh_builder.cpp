#include "navmesh_builder.h"
#include <fstream>
#include <iostream>
#include <cstring>
#include <cmath>

// Detour включається тільки якщо src/recast/Detour існує
#ifdef HAVE_RECAST
#include "recast/Detour/Include/DetourNavMesh.h"
#include "recast/Detour/Include/DetourNavMeshQuery.h"
#include "recast/Detour/Include/DetourCommon.h"
#include "recast/Detour/Include/DetourAlloc.h"
#endif

NavMeshBuilder::NavMeshBuilder() {
#ifdef HAVE_RECAST
    m_filter = new dtQueryFilter();
    m_filter->setIncludeFlags(0xFFFF);
    m_filter->setExcludeFlags(0);
#endif
}

NavMeshBuilder::~NavMeshBuilder() {
#ifdef HAVE_RECAST
    dtFreeNavMeshQuery(m_query);
    dtFreeNavMesh(m_mesh);
    delete m_filter;
#endif
}

bool NavMeshBuilder::IsValid() const {
#ifdef HAVE_RECAST
    return m_mesh != nullptr && m_query != nullptr;
#else
    return false;
#endif
}

bool NavMeshBuilder::Load(const std::string& path) {
#ifndef HAVE_RECAST
    std::cerr << "[NAVMESH] Recast/Detour не знайдено (HAVE_RECAST не визначено)\n";
    return false;
#else
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::cerr << "[NAVMESH] Cannot open: " << path << "\n";
        return false;
    }

    uint32_t magic = 0, size = 0;
    f.read((char*)&magic, 4);
    f.read((char*)&size,  4);

    if (magic != 0x4E4D5348u || size == 0) {
        std::cerr << "[NAVMESH] Bad format: " << path << "\n";
        return false;
    }

    // Читаємо tile data в буфер що буде керуватись dtNavMesh (DT_TILE_FREE_DATA)
    uint8_t* data = (uint8_t*)dtAlloc(size, DT_ALLOC_PERM);
    if (!data) return false;
    f.read((char*)data, size);
    if (!f) { dtFree(data); return false; }

    // Якщо вже є mesh — звільняємо
    if (m_mesh) { dtFreeNavMesh(m_mesh); m_mesh = nullptr; }
    if (m_query){ dtFreeNavMeshQuery(m_query); m_query = nullptr; }

    m_mesh = dtAllocNavMesh();
    if (!m_mesh) { dtFree(data); return false; }

    // Single-tile: init + addTile
    dtNavMeshParams mp{};
    memset(&mp, 0, sizeof(mp));
    mp.maxTiles = 1;
    mp.maxPolys = 32768;

    dtStatus st = m_mesh->init(&mp);
    if (dtStatusFailed(st)) {
        dtFreeNavMesh(m_mesh); m_mesh = nullptr; dtFree(data); return false;
    }

    dtTileRef ref = 0;
    st = m_mesh->addTile(data, (int)size, DT_TILE_FREE_DATA, 0, &ref);
    if (dtStatusFailed(st) || ref == 0) {
        std::cerr << "[NAVMESH] addTile failed: " << path << "\n";
        dtFreeNavMesh(m_mesh); m_mesh = nullptr; return false;
    }

    m_query = dtAllocNavMeshQuery();
    if (!m_query) { dtFreeNavMesh(m_mesh); m_mesh = nullptr; return false; }

    st = m_query->init(m_mesh, 2048);
    if (dtStatusFailed(st)) {
        std::cerr << "[NAVMESH] Query init failed\n"; return false;
    }

    std::cerr << "[NAVMESH] Завантажено: " << path << "\n";
    return true;
#endif
}

bool NavMeshBuilder::IsWalkable(float x, float y, float z) const {
#ifndef HAVE_RECAST
    return true;
#else
    if (!IsValid()) return true;
    float dx, dy, dz;
    toDetour(x, y, z, dx, dy, dz);
    float pos[3] = {dx, dy, dz};
    float ext[3] = {SEARCH_EXT_XZ, SEARCH_EXT_Y, SEARCH_EXT_XZ};
    dtPolyRef poly;
    float nearest[3];
    dtStatus st = m_query->findNearestPoly(pos, ext, m_filter, &poly, nearest);
    return dtStatusSucceed(st) && poly != 0;
#endif
}

std::vector<std::pair<float,float>> NavMeshBuilder::FindPath(
        float sx, float sy, float sz,
        float ex, float ey, float ez) {
#ifndef HAVE_RECAST
    return {};
#else
    if (!IsValid()) return {};

    float dsx, dsy, dsz, dex, dey, dez;
    toDetour(sx, sy, sz, dsx, dsy, dsz);
    toDetour(ex, ey, ez, dex, dey, dez);

    float spos[3] = {dsx, dsy, dsz};
    float epos[3] = {dex, dey, dez};
    float ext[3]  = {SEARCH_EXT_XZ, SEARCH_EXT_Y, SEARCH_EXT_XZ};

    dtPolyRef start_ref, end_ref;
    float start_pt[3], end_pt[3];

    dtStatus st = m_query->findNearestPoly(spos, ext, m_filter,
                                            &start_ref, start_pt);
    if (dtStatusFailed(st) || start_ref == 0) return {};

    st = m_query->findNearestPoly(epos, ext, m_filter,
                                   &end_ref, end_pt);
    if (dtStatusFailed(st) || end_ref == 0) return {};

    // findPath — коридор полігонів
    dtPolyRef polys[MAX_POLYS];
    int npolys = 0;
    st = m_query->findPath(start_ref, end_ref, start_pt, end_pt,
                            m_filter, polys, &npolys, MAX_POLYS);
    if (dtStatusFailed(st) || npolys == 0) return {};

    // findStraightPath — waypoints вздовж коридору
    float smooth[MAX_SMOOTH * 3];
    uint8_t flags[MAX_SMOOTH];
    dtPolyRef refs[MAX_SMOOTH];
    int nsmooth = 0;

    st = m_query->findStraightPath(start_pt, end_pt,
                                    polys, npolys,
                                    smooth, flags, refs,
                                    &nsmooth, MAX_SMOOTH,
                                    DT_STRAIGHTPATH_ALL_CROSSINGS);
    if (dtStatusFailed(st) || nsmooth == 0) return {};

    // Конвертуємо Detour → L2 (пропускаємо стартову точку)
    std::vector<std::pair<float,float>> result;
    result.reserve(nsmooth);
    for (int i = 1; i < nsmooth; i++) {
        float lx, ly, lz;
        fromDetour(smooth[i*3], smooth[i*3+1], smooth[i*3+2], lx, ly, lz);
        result.emplace_back(lx, ly);
    }
    return result;
#endif
}
