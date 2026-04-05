#pragma once
#include <string>
#include <vector>
#include <utility>
#include <cstdint>

// Forward declarations — не включаємо Detour headers у .h
struct dtNavMesh;
struct dtNavMeshQuery;
struct dtQueryFilter;

// ── NavMeshBuilder ────────────────────────────────────────────────────────────
// Runtime pathfinding через Detour. Завантажує .bin з build_navmesh.
// Тільки Detour (6 .cpp файлів) — Recast не потрібен в runtime.
//
// Координати: L2 (X=East, Y=North, Z=вгора).
// Внутрішньо: Detour (x=X, y=Z, z=Y) — L2 Y ↔ Detour Z.
class NavMeshBuilder {
public:
    NavMeshBuilder();
    ~NavMeshBuilder();

    // Завантажити .bin файл (формат: magic + size + tile data)
    bool Load(const std::string& path);

    // Пошук шляху від (sx,sy,sz) до (ex,ey,ez) в L2 координатах.
    // Повертає waypoints (x,y) або {} якщо не знайдено.
    // < 1мс на типовому маршруті.
    std::vector<std::pair<float,float>> FindPath(
        float sx, float sy, float sz,
        float ex, float ey, float ez);

    // Перевірка чи точка прохідна (є поблизу NavMesh полігон)
    bool IsWalkable(float x, float y, float z) const;

    bool IsValid() const;

private:
    dtNavMesh*      m_mesh   = nullptr;
    dtNavMeshQuery* m_query  = nullptr;
    dtQueryFilter*  m_filter = nullptr;

    // L2 → Detour: swap Y↔Z (Detour: Y=вгора)
    static void toDetour(float lx, float ly, float lz,
                          float& dx, float& dy, float& dz) {
        dx = lx; dy = lz; dz = ly;
    }
    static void fromDetour(float dx, float dy, float dz,
                            float& lx, float& ly, float& lz) {
        lx = dx; ly = dz; lz = dy;
    }

    static constexpr int MAX_POLYS  = 256;
    static constexpr int MAX_SMOOTH = 512;

    // Extents для findNearestPoly (половина розміру search box)
    // 300 L2u по X/Z, 200 L2u по Y (висота)
    static constexpr float SEARCH_EXT_XZ = 300.f;
    static constexpr float SEARCH_EXT_Y  = 200.f;
};
