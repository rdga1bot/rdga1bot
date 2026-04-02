#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <utility>

// ── Geodata ───────────────────────────────────────────────────────────────────
// Завантаження L2J-формату геодати та навігація на основі NSWE-прапорів.
//
// Формат файлу XX_YY.geo (L2J Interlude/Kamael):
//   - Заголовок: uint32_t version (=1)
//   - 256×256 × uint16_t nswe = 131072 байти
//     bits: N=0x01, S=0x02, W=0x04, E=0x08
//   - Після масиву: Z-chunks для кожного блоку з ненульовим NSWE
//     кількість chunks = popcount(nswe & 0x0F)
//     кожен chunk: int16_t zMin, int16_t zMax
//
// Координатна система L2:
//   X: -327680..327680  (East +)
//   Y: -327680..327680  (North +)
//   Block size: 256 units (2560 blocks per axis, 10 regions per axis)
//   Region: 256×256 blocks, файл XX_YY.geo
//
// A* pathfinding:
//   - Крок сітки: GEODATA_STEP (256 units = 1 block)
//   - 4-connected (N/S/E/W)
//   - Евристика: Manhattan
//   - Макс. вузлів: GEO_MAX_NODES (500)
//   - Асинхронний виклик з таймаутом 50мс (FindPath)

class Geodata {
public:
    // ── Константи ────────────────────────────────────────────────────────────
    static constexpr int   GEO_WORLD_MIN  = -327680;   // мінімальна координата
    static constexpr int   GEO_BLOCK_SIZE = 256;        // юнітів на блок
    static constexpr int   GEO_REGION_BLOCKS = 256;     // блоків на регіон
    static constexpr int   GEO_MAX_NODES  = 500;        // A* ліміт вузлів
    static constexpr int   GEO_PATH_TIMEOUT_MS = 50;    // таймаут A* в мс

    // NSWE bits
    static constexpr uint16_t GEO_N = 0x01;
    static constexpr uint16_t GEO_S = 0x02;
    static constexpr uint16_t GEO_W = 0x04;
    static constexpr uint16_t GEO_E = 0x08;

    Geodata() = default;

    // Завантажити всі .geo файли з каталогу path.
    // useJPS = true → увімкнути Jump Point Search (наразі заглушка, A* в будь-якому разі)
    // Повертає кількість завантажених регіонів.
    int Load(const std::string& path, bool useJPS = true);

    // Перевірити чи можна потрапити в точку (x, y, z).
    // Повертає false якщо блок не завантажений або NSWE==0.
    bool CanMoveTo(float x, float y, float z = 0.f) const;

    // Перевірити наявність стіни між двома точками (2D Bresenham по блоках).
    bool IsWallBetween(float x1, float y1, float z1,
                       float x2, float y2, float z2) const;

    // Те саме, 2D версія
    bool IsWallBetween2D(float x1, float y1, float x2, float y2) const;

    // Перевірка прямої видимості (alias для !IsWallBetween)
    bool IsVisible(float x1, float y1, float z1,
                   float x2, float y2, float z2) const {
        return !IsWallBetween(x1, y1, z1, x2, y2, z2);
    }

    // A* пошук шляху від (sx,sy) до (ex,ey).
    // Повертає вектор проміжних точок (world coords) без стартової точки.
    // Якщо шлях не знайдено або регіони не завантажені → порожній вектор.
    // Виконується в поточному потоці (не асинхронно) з лімітом GEO_MAX_NODES.
    std::vector<std::pair<float,float>> FindPath(
        float sx, float sy, float sz,
        float ex, float ey, float ez,
        float maxRange = 5000.f) const;

    // Перевірити чи завантажені геодані для регіону що містить точку (x, y)
    bool IsLoaded(float x, float y) const;

    // Кількість завантажених регіонів
    int RegionCount() const { return (int)m_regions.size(); }

private:
    // ── Внутрішні типи ────────────────────────────────────────────────────────

    // Ключ регіону: (regionX << 8) | regionY
    using RegionKey = uint32_t;

    struct Region {
        int rx, ry;                   // координати регіону
        std::vector<uint16_t> nswe;   // 256×256 NSWE values, row-major [blockX * 256 + blockY]
    };

    // ── Дані ─────────────────────────────────────────────────────────────────
    std::unordered_map<RegionKey, Region> m_regions;
    bool m_use_jps = true;

    // ── Конвертація координат ─────────────────────────────────────────────────

    // World coords → block indices (global, 0..2559)
    static int WorldToBlock(float w) {
        return (int)((w - GEO_WORLD_MIN) / GEO_BLOCK_SIZE);
    }

    // Block index → world coord (центр блоку)
    static float BlockToWorld(int b) {
        return (float)(GEO_WORLD_MIN + b * GEO_BLOCK_SIZE + GEO_BLOCK_SIZE / 2);
    }

    // Block → region index
    static int BlockToRegion(int b) { return b / GEO_REGION_BLOCKS; }

    // Block → local block within region
    static int BlockToLocal(int b)  { return b % GEO_REGION_BLOCKS; }

    // Region key
    static RegionKey MakeKey(int rx, int ry) {
        return (RegionKey)(((uint32_t)rx << 8) | (uint32_t)ry);
    }

    // ── Доступ до NSWE ────────────────────────────────────────────────────────

    // NSWE для блоку (global block coords)
    // Повертає 0 якщо регіон не завантажений
    uint16_t GetNSWE(int bx, int by) const;

    // Перевірка чи можна вийти з блоку (gbx, gby) у напрямку dir
    bool CanExit(int gbx, int gby, uint16_t dir) const;

    // Перевірка чи можна увійти в блок (gbx, gby) з напрямку dir
    // (перевіряємо протилежний bit в destination)
    bool CanEnter(int gbx, int gby, uint16_t from_dir) const;

    // ── A* ───────────────────────────────────────────────────────────────────

    struct ANode {
        int bx, by;
        float g, h;
        float f() const { return g + h; }
        int parent_bx, parent_by;
        bool operator>(const ANode& o) const { return f() > o.f(); }
    };

    // Зворотній напрямок (для CanEnter)
    static uint16_t Opposite(uint16_t dir);
};
