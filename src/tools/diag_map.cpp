// tools/diag_map.cpp
// NavMesh point recording mode: --map
// Веди персонажа вручну навколо перешкод — записуються точки прибуття.
// Ctrl+C → зберігає navmesh_points.pts (додає до існуючого файлу).
#include "diag.h"
#include "../Config.h"
#include "../offset_scanner.h"
#include "../offsets_config.h"
#include <iostream>
#include <fstream>
#include <vector>
#include <array>
#include <string>
#include <csignal>
#include <thread>
#include <chrono>
#include <iomanip>
#include <cstdlib>

void runMapMode(const std::string& config_path, uintptr_t override_pb) {
    Config cfg; cfg.Load(config_path);

    const std::string pts_file = cfg.navmesh_cfg.points_file.empty()
                               ? "navmesh_points.pts"
                               : cfg.navmesh_cfg.points_file;
    const float collect_dist = cfg.navmesh_cfg.collect_dist > 0.f
                             ? cfg.navmesh_cfg.collect_dist : 30.f;

    pid_t pid = findL2Pid();
    if (!pid) { std::cerr << "[MAP] l2.exe не знайдено\n"; return; }

    OffsetScanner scanner(pid);
    scanner.loadOffsets(cfg.knownlist_offsets_file);

    uintptr_t playerBase = override_pb;
    if (!playerBase) {
        std::cerr << "[MAP] blindScan()...\n";
        playerBase = scanner.blindScan();
        if (!playerBase) {
            std::cerr << "[MAP] PlayerBase не знайдено\n"
                      << "[MAP] Спробуй: ./rdga1bot --find-pos  →  ./rdga1bot --map --pb 0xADDR\n";
            return;
        }
    }
    float _cx = scanner.rpm_pub<float>(playerBase + OFF_PLAYER_X);
    float _cy = scanner.rpm_pub<float>(playerBase + OFF_PLAYER_Y);
    float _cz = scanner.rpm_pub<float>(playerBase + 0x2C);
    std::cerr << "[MAP] PlayerBase=0x" << std::hex << playerBase << std::dec
              << "  поточна позиція: X=" << (int)_cx
              << " Y=" << (int)_cy << " Z=" << (int)_cz << "\n"
              << "[MAP] Якщо координати невірні → запусти --find-pos, отримай правильний pb\n";

    // Завантажуємо існуючі точки
    std::vector<std::array<float,3>> pts;
    {
        std::ifstream fin(pts_file, std::ios::binary);
        if (fin) {
            uint32_t cnt = 0;
            fin.read(reinterpret_cast<char*>(&cnt), 4);
            pts.resize(cnt);
            for (auto& p : pts) fin.read(reinterpret_cast<char*>(p.data()), 12);
            std::cerr << "[MAP] Завантажено " << cnt << " існуючих точок з " << pts_file << "\n";
        }
    }

    float last_x = 0.f, last_y = 0.f;
    bool  have_last = false;

    // static local для Ctrl+C handler
    static std::vector<std::array<float,3>>* g_pts_ptr = nullptr;
    static std::string                        g_pts_file;
    g_pts_ptr  = &pts;
    g_pts_file = pts_file;
    std::signal(SIGINT, [](int) {
        if (!g_pts_ptr) _exit(0);
        std::ofstream fout(g_pts_file, std::ios::binary | std::ios::trunc);
        uint32_t cnt = (uint32_t)g_pts_ptr->size();
        fout.write(reinterpret_cast<const char*>(&cnt), 4);
        for (auto& p : *g_pts_ptr) fout.write(reinterpret_cast<const char*>(p.data()), 12);
        fout.flush();
        fout.close();
        std::cerr << "\n[MAP] Збережено " << cnt << " точок → " << g_pts_file << "\n";
        std::cerr << "[MAP] Тепер збудуй mesh:\n"
                  << "  ./tools/build_navmesh " << g_pts_file << " navmesh.bin\n";
        _exit(0);
    });

    std::cerr << "[MAP] Веди персонажа вручну навколо перешкод. Ctrl+C щоб зберегти.\n";
    std::cerr << "[MAP] dist=" << collect_dist << " L2u  |  точок: " << pts.size() << "\n\n";

    while (true) {
        float px = scanner.rpm_pub<float>(playerBase + OFF_PLAYER_X);
        float py = scanner.rpm_pub<float>(playerBase + OFF_PLAYER_Y);
        float pz = scanner.rpm_pub<float>(playerBase + OFF_PLAYER_Z);

        bool valid = (px > -327000.f && px < 327000.f &&
                      py > -327000.f && py < 327000.f &&
                      pz > -16384.f  && pz < 16384.f  &&
                      !(px == 0.f && py == 0.f));

        if (valid) {
            float dx = px - last_x, dy = py - last_y;
            float dist2 = dx*dx + dy*dy;
            if (!have_last || dist2 >= collect_dist * collect_dist) {
                pts.push_back({px, py, pz});
                last_x = px; last_y = py; have_last = true;
                std::cerr << "\r[MAP] #" << std::setw(4) << pts.size()
                          << "  X=" << std::setw(8) << (int)px
                          << " Y=" << std::setw(8) << (int)py
                          << " Z=" << std::setw(6) << (int)pz
                          << "          " << std::flush;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}
