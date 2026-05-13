#include "Geodata.h"

#include <fstream>
#include <sstream>
#include <iostream>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <queue>
#include <unordered_map>
#include <chrono>

// ─── Load ──────────────────────────────────────────────────────────────────
// Скануємо каталог на файли вигляду XX_YY.geo або XX_YY.l2j
// та завантажуємо кожен регіон.

#include <dirent.h>   // POSIX directory listing

int Geodata::Load(const std::string& path, bool useJPS) {
    m_use_jps = useJPS;
    m_regions.clear();

    DIR* dir = opendir(path.c_str());
    if (!dir) {
        std::cerr << "[GEODATA] Не вдалося відкрити каталог: " << path << "\n";
        return 0;
    }

    int loaded = 0;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string fname(entry->d_name);
        // Шукаємо файли вигляду "XX_YY.geo"
        if (fname.size() < 6) continue;
        std::string ext = fname.substr(fname.size() - 4);
        // Перетворюємо в нижній регістр
        std::string ext_lower = ext;
        for (auto& c : ext_lower) c = (char)tolower((unsigned char)c);
        if (ext_lower != ".geo") continue;

        // Парсимо "XX_YY.geo"
        std::string base = fname.substr(0, fname.size() - 4);
        size_t underscore = base.find('_');
        if (underscore == std::string::npos) continue;

        int rx, ry;
        try {
            rx = std::stoi(base.substr(0, underscore));
            ry = std::stoi(base.substr(underscore + 1));
        } catch (...) { continue; }

        std::string full_path = path + "/" + fname;
        std::ifstream f(full_path, std::ios::binary);
        if (!f.is_open()) {
            std::cerr << "[GEODATA] Не вдалося відкрити: " << full_path << "\n";
            continue;
        }

        // Читаємо заголовок: 4 байти (версія)
        uint32_t version = 0;
        f.read(reinterpret_cast<char*>(&version), 4);
        if (f.fail()) {
            std::cerr << "[GEODATA] " << fname << ": помилка читання заголовку\n";
            continue;
        }

        if (version != 1) {
            std::cerr << "[GEODATA] " << fname << ": невідома версія " << version
                      << " (очікується 1)\n";
            // Спробуємо продовжити — деякі клієнти мають інші версії
        }

        // Читаємо 256×256 NSWE values (2 байти кожне)
        constexpr int BLOCKS = GEO_REGION_BLOCKS * GEO_REGION_BLOCKS; // 65536
        Region region;
        region.rx = rx;
        region.ry = ry;
        region.nswe.resize(BLOCKS);

        f.read(reinterpret_cast<char*>(region.nswe.data()),
               BLOCKS * sizeof(uint16_t));
        if (f.fail()) {
            // Файл може бути без заголовку — спробуємо читати без version skip
            f.clear();
            f.seekg(0, std::ios::beg);
            f.read(reinterpret_cast<char*>(region.nswe.data()),
                   BLOCKS * sizeof(uint16_t));
            if (f.fail()) {
                std::cerr << "[GEODATA] " << fname << ": недостатньо даних\n";
                continue;
            }
        }

        // Z-chunks пропускаємо (не потрібні для 2D навігації)

        RegionKey key = MakeKey(rx, ry);
        m_regions[key] = std::move(region);
        loaded++;
        std::cerr << "[GEODATA] Завантажено " << fname
                  << " (region " << rx << "," << ry << ")\n";
    }
    closedir(dir);

    if (loaded == 0) {
        std::cerr << "[GEODATA] Жодного .geo файлу не знайдено в: " << path << "\n";
    } else {
        std::cerr << "[GEODATA] Завантажено " << loaded << " регіонів\n";
    }
    return loaded;
}

// ─── GetNSWE ──────────────────────────────────────────────────────────────────
uint16_t Geodata::GetNSWE(int bx, int by) const {
    int rx = BlockToRegion(bx);
    int ry = BlockToRegion(by);
    auto it = m_regions.find(MakeKey(rx, ry));
    if (it == m_regions.end()) return 0; // не завантажено → пропуск
    int lx = BlockToLocal(bx);
    int ly = BlockToLocal(by);
    int idx = lx * GEO_REGION_BLOCKS + ly;
    return it->second.nswe[(size_t)idx];
}

// ─── CanExit / CanEnter ───────────────────────────────────────────────────────
bool Geodata::CanExit(int gbx, int gby, uint16_t dir) const {
    return (GetNSWE(gbx, gby) & dir) != 0;
}

uint16_t Geodata::Opposite(uint16_t dir) {
    switch (dir) {
        case GEO_N: return GEO_S;
        case GEO_S: return GEO_N;
        case GEO_W: return GEO_E;
        case GEO_E: return GEO_W;
        default: return 0;
    }
}

bool Geodata::CanEnter(int gbx, int gby, uint16_t from_dir) const {
    return (GetNSWE(gbx, gby) & Opposite(from_dir)) != 0;
}

// ─── IsLoaded ────────────────────────────────────────────────────────────────
bool Geodata::IsLoaded(float x, float y) const {
    int bx = WorldToBlock(x);
    int by = WorldToBlock(y);
    int rx = BlockToRegion(bx);
    int ry = BlockToRegion(by);
    return m_regions.find(MakeKey(rx, ry)) != m_regions.end();
}

// ─── CanMoveTo ────────────────────────────────────────────────────────────────
bool Geodata::CanMoveTo(float x, float y, float /*z*/) const {
    int bx = WorldToBlock(x);
    int by = WorldToBlock(y);
    uint16_t nswe = GetNSWE(bx, by);
    // Якщо регіон не завантажений (nswe=0 via fallback) — вважаємо прохідним
    int rx = BlockToRegion(bx);
    int ry = BlockToRegion(by);
    if (m_regions.find(MakeKey(rx, ry)) == m_regions.end()) return true;
    // Блок прохідний якщо є хоч один вихідний напрям
    return (nswe & (GEO_N | GEO_S | GEO_E | GEO_W)) != 0;
}

// ─── IsWallBetween2D ─────────────────────────────────────────────────────────
// Bresenham по блоках: крокуємо від (x1,y1) до (x2,y2) і перевіряємо кожен перехід.
bool Geodata::IsWallBetween2D(float x1, float y1, float x2, float y2) const {
    int bx1 = WorldToBlock(x1);
    int by1 = WorldToBlock(y1);
    int bx2 = WorldToBlock(x2);
    int by2 = WorldToBlock(y2);

    int dx = std::abs(bx2 - bx1);
    int dy = std::abs(by2 - by1);
    int sx = (bx1 < bx2) ? 1 : -1;
    int sy = (by1 < by2) ? 1 : -1;
    int err = dx - dy;

    int cx = bx1, cy = by1;
    int max_steps = dx + dy + 1;

    for (int step = 0; step < max_steps && (cx != bx2 || cy != by2); step++) {
        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            // Рухаємось по X
            uint16_t dir = (sx > 0) ? GEO_E : GEO_W;
            if (!CanExit(cx, cy, dir)) return true; // стіна
            cx += sx;
            if (!CanEnter(cx, cy, dir)) return true;
        }
        if (e2 < dx) {
            err += dx;
            // Рухаємось по Y
            uint16_t dir = (sy > 0) ? GEO_N : GEO_S;
            if (!CanExit(cx, cy, dir)) return true;
            cy += sy;
            if (!CanEnter(cx, cy, dir)) return true;
        }
    }
    return false;
}

bool Geodata::IsWallBetween(float x1, float y1, float /*z1*/,
                             float x2, float y2, float /*z2*/) const {
    return IsWallBetween2D(x1, y1, x2, y2);
}

// ── Спільна утиліта: спрощення collinear точок ───────────────────────────────
static std::vector<std::pair<float,float>> SimplifyPath(
        std::vector<std::pair<float,float>> path)
{
    if (path.size() <= 2) return path;
    std::vector<std::pair<float,float>> out;
    out.push_back(path[0]);
    for (size_t i = 1; i + 1 < path.size(); i++) {
        auto [ax, ay] = out.back();
        auto [bx2, by2] = path[i];
        auto [cx2, cy2] = path[i+1];
        float cross = (bx2-ax)*(cy2-ay) - (by2-ay)*(cx2-ax);
        if (std::fabs(cross) > 0.01f) out.push_back(path[i]);
    }
    out.push_back(path.back());
    return out;
}

// ─── FindPath — dispatcher ────────────────────────────────────────────────────
std::vector<std::pair<float,float>>
Geodata::FindPath(float sx, float sy, float /*sz*/,
                  float ex, float ey, float /*ez*/,
                  float maxRange) const
{
    int sbx = WorldToBlock(sx), sby = WorldToBlock(sy);
    int ebx = WorldToBlock(ex), eby = WorldToBlock(ey);
    if (sbx == ebx && sby == eby) return {};

    float world_dist = std::sqrt((ex-sx)*(ex-sx) + (ey-sy)*(ey-sy));
    if (world_dist > maxRange) return {};

    auto raw = m_use_jps
        ? FindPathJPS(sbx, sby, ebx, eby)
        : FindPathAStar(sbx, sby, ebx, eby);

    return SimplifyPath(std::move(raw));
}

// ─── FindPathAStar ────────────────────────────────────────────────────────────
std::vector<std::pair<float,float>>
Geodata::FindPathAStar(int sbx, int sby, int ebx, int eby) const
{
    auto t_start = std::chrono::steady_clock::now();

    auto NodeKey = [](int bx, int by) -> int64_t {
        return ((int64_t)(bx + 4096) << 16) | (int64_t)(by + 4096);
    };
    auto Heuristic = [&](int bx, int by) -> float {
        return (float)(std::abs(bx - ebx) + std::abs(by - eby));
    };

    std::unordered_map<int64_t, float>   g_cost;
    std::unordered_map<int64_t, int64_t> parent;
    using QNode = std::tuple<float, int, int>;
    std::priority_queue<QNode, std::vector<QNode>, std::greater<QNode>> open;

    int64_t start_key = NodeKey(sbx, sby);
    g_cost[start_key] = 0.f;
    open.push({Heuristic(sbx, sby), sbx, sby});
    parent[start_key] = -1;

    struct Dir { int dbx, dby; uint16_t bit; };
    static const Dir dirs[4] = {
        { 0, +1, GEO_N }, { 0, -1, GEO_S },
        {-1,  0, GEO_W }, {+1,  0, GEO_E },
    };

    int nodes_explored = 0;
    bool found = false;

    while (!open.empty() && nodes_explored < GEO_MAX_NODES) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t_start).count();
        if (elapsed > GEO_PATH_TIMEOUT_MS) break;

        auto [f, cx, cy] = open.top(); open.pop();
        nodes_explored++;
        int64_t cur_key = NodeKey(cx, cy);
        if (cx == ebx && cy == eby) { found = true; break; }

        float cur_g = g_cost.count(cur_key) ? g_cost[cur_key] : 1e9f;
        for (const auto& d : dirs) {
            int nx = cx + d.dbx, ny = cy + d.dby;
            if (!CanExit(cx, cy, d.bit)) continue;
            {
                int rx = BlockToRegion(nx), ry = BlockToRegion(ny);
                if (m_regions.count(MakeKey(rx, ry)) && !CanEnter(nx, ny, d.bit)) continue;
            }
            float new_g = cur_g + 1.f;
            int64_t nkey = NodeKey(nx, ny);
            if (!g_cost.count(nkey) || new_g < g_cost[nkey]) {
                g_cost[nkey] = new_g;
                parent[nkey] = cur_key;
                open.push({new_g + Heuristic(nx, ny), nx, ny});
            }
        }
    }

    if (!found) return {};

    std::vector<std::pair<float,float>> path;
    int64_t cur = NodeKey(ebx, eby);
    int64_t sentinel = -1;
    while (cur != sentinel && parent.count(cur)) {
        int by = (int)(cur & 0xFFFF) - 4096;
        int bx = (int)((cur >> 16) & 0xFFFF) - 4096;
        path.push_back({ BlockToWorld(bx), BlockToWorld(by) });
        int64_t prev = parent[cur];
        if (prev == cur) break;
        cur = prev;
    }
    std::reverse(path.begin(), path.end());
    if (!path.empty()) path.erase(path.begin());
    return path;
}

// ─── JumpJPS ─────────────────────────────────────────────────────────────────
std::pair<int,int> Geodata::JumpJPS(int bx, int by,
                                     int dx, int dy,
                                     int ebx, int eby) const
{
    // Визначаємо NSWE bit для напряму
    uint16_t dir_bit;
    if      (dx > 0) dir_bit = GEO_E;
    else if (dx < 0) dir_bit = GEO_W;
    else if (dy > 0) dir_bit = GEO_N;
    else             dir_bit = GEO_S;

    static constexpr int MAX_JUMP = 128;
    int cx = bx, cy = by;

    for (int step = 0; step < MAX_JUMP; step++) {
        if (!CanExit(cx, cy, dir_bit)) return {-1, -1};
        int nx = cx + dx, ny = cy + dy;

        if (nx == ebx && ny == eby) return {nx, ny};

        // Forced neighbor detection для 4-connected:
        // Obstacle в перпендикулярному напрямку з cx,cy але не з nx,ny → jump point
        if (dx != 0) {
            if ((!CanExit(cx, cy, GEO_N) && CanExit(nx, ny, GEO_N)) ||
                (!CanExit(cx, cy, GEO_S) && CanExit(nx, ny, GEO_S)))
                return {nx, ny};
        } else {
            if ((!CanExit(cx, cy, GEO_E) && CanExit(nx, ny, GEO_E)) ||
                (!CanExit(cx, cy, GEO_W) && CanExit(nx, ny, GEO_W)))
                return {nx, ny};
        }
        cx = nx; cy = ny;
    }
    return {-1, -1};
}

// ─── FindPathJPS ──────────────────────────────────────────────────────────────
std::vector<std::pair<float,float>>
Geodata::FindPathJPS(int sbx, int sby, int ebx, int eby) const
{
    auto t_start = std::chrono::steady_clock::now();

    auto Key = [](int bx, int by) -> int64_t {
        return ((int64_t)(bx + 4096) << 16) | (int64_t)(by + 4096);
    };
    auto H = [&](int bx, int by) -> float {
        return (float)(std::abs(bx - ebx) + std::abs(by - eby));
    };

    struct NodeInfo { float g = 1e9f; int pbx = -1, pby = -1; bool closed = false; };
    std::unordered_map<int64_t, NodeInfo> nodes;
    nodes.reserve(256);

    using QNode = std::tuple<float, int, int, int, int>; // f, bx, by, dx, dy
    std::priority_queue<QNode, std::vector<QNode>, std::greater<QNode>> open;

    nodes[Key(sbx, sby)].g    = 0.f;
    nodes[Key(sbx, sby)].pbx  = sbx;
    nodes[Key(sbx, sby)].pby  = sby;

    // Старт: всі 4 напрямки
    const int dirs[4][2] = {{0,1},{0,-1},{-1,0},{1,0}};
    for (auto& d : dirs)
        open.push({H(sbx, sby), sbx, sby, d[0], d[1]});

    bool found = false;
    int explored = 0;

    while (!open.empty() && explored < GEO_MAX_NODES) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t_start).count();
        if (elapsed > GEO_PATH_TIMEOUT_MS) break;

        auto [f, cx, cy, dx, dy] = open.top(); open.pop();
        int64_t cur_key = Key(cx, cy);
        auto& cur_node = nodes[cur_key];
        if (cur_node.closed) continue;
        cur_node.closed = true;
        explored++;

        if (cx == ebx && cy == eby) { found = true; break; }

        float cur_g = cur_node.g;

        // Знаходимо jump point у поточному напрямку
        auto [jx, jy] = JumpJPS(cx, cy, dx, dy, ebx, eby);
        if (jx >= 0) {
            float jg = cur_g + std::sqrt((float)((jx-cx)*(jx-cx)+(jy-cy)*(jy-cy)));
            int64_t jkey = Key(jx, jy);
            auto& jn = nodes[jkey];
            if (jg < jn.g) {
                jn.g = jg; jn.pbx = cx; jn.pby = cy;
                open.push({jg + H(jx, jy), jx, jy, dx, dy});
            }

            // Перпендикулярні напрямки від jump point
            for (auto& nd : dirs) {
                if ((nd[0] == dx && nd[1] == dy) || (nd[0] == -dx && nd[1] == -dy)) continue;
                auto [nx2, ny2] = JumpJPS(jx, jy, nd[0], nd[1], ebx, eby);
                if (nx2 < 0) continue;
                float ng = jg + std::sqrt((float)((nx2-jx)*(nx2-jx)+(ny2-jy)*(ny2-jy)));
                int64_t nkey = Key(nx2, ny2);
                auto& nn = nodes[nkey];
                if (ng < nn.g) {
                    nn.g = ng; nn.pbx = jx; nn.pby = jy;
                    open.push({ng + H(nx2, ny2), nx2, ny2, nd[0], nd[1]});
                }
            }
        }
    }

    if (!found) return {};

    // Реконструкція шляху
    std::vector<std::pair<float,float>> path;
    int cx = ebx, cy = eby;
    for (int safety = 0; safety < GEO_MAX_NODES; safety++) {
        path.push_back({BlockToWorld(cx), BlockToWorld(cy)});
        int64_t key = Key(cx, cy);
        if (!nodes.count(key)) break;
        int px = nodes[key].pbx, py = nodes[key].pby;
        if (px == cx && py == cy) break;
        cx = px; cy = py;
    }
    std::reverse(path.begin(), path.end());
    if (!path.empty()) path.erase(path.begin());
    return path;
}
