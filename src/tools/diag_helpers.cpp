// tools/diag_helpers.cpp
// Спільні утиліти для CLI-діагностичних функцій.
#include "diag.h"
#include "../platform.h"
#include <string>
#include <fstream>
#include <cctype>
#ifndef _WIN32
#include <dirent.h>
#endif
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#endif

pid_t findL2Pid() {
#ifdef _WIN32
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32 pe = {};
    pe.dwSize = sizeof(pe);
    pid_t found = 0;
    if (Process32First(snap, &pe)) {
        do {
            char name[MAX_PATH];
            strncpy(name, pe.szExeFile, sizeof(name) - 1);
            for (auto& c : name) c = (char)tolower((unsigned char)c);
            if (std::string(name) == "l2.exe") { found = pe.th32ProcessID; break; }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return found;
#else
    DIR* dir = opendir("/proc");
    if (!dir) return 0;
    struct dirent* e;
    pid_t found = 0;
    while ((e = readdir(dir)) != nullptr && !found) {
        bool num = true;
        for (char c : std::string(e->d_name))
            if (!isdigit(c)) { num = false; break; }
        if (!num) continue;
        std::ifstream cmd("/proc/" + std::string(e->d_name) + "/cmdline");
        std::string line; std::getline(cmd, line, '\0');
        auto p = line.rfind('/');
        std::string exe = (p != std::string::npos) ? line.substr(p+1) : line;
        auto lc = [](std::string s){ for(auto& c:s) c=(char)tolower((unsigned char)c); return s; };
        if (lc(exe) == "l2.exe") found = (pid_t)std::stoi(e->d_name);
    }
    closedir(dir);
    return found;
#endif
}
