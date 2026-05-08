// tools/diag.h
// Forward declarations CLI-діагностичних функцій (--dump-objects, --calibrate тощо).
// Включати тільки в main.cpp.
// НЕ включати в bot runtime файли.
#pragma once
#include <string>
#include <cstdint>
#include "../platform.h"  // pid_t

// helpers
pid_t findL2Pid();

// diag_dump.cpp
void runDumpObjects  (const std::string& config_path);
void runDiscoverKlist(const std::string& config_path);

// diag_calibrate.cpp
void runCalibrate     (const std::string& config_path, int argc, char* argv[]);
void runHeadingMonitor(const std::string& config_path);
void runHpCalibrate   (const std::string& config_path);

// diag_findpos.cpp
void runFindPos  (const std::string& config_path);
void runWatchPos (const std::string& config_path, uintptr_t override_pb);
void runDiffScan (const std::string& config_path);
void runScanPos  (const std::string& config_path);
void runDumpGobj (const std::string& config_path); // dump game_obj для пошуку HP offset
void runScanHp   (const std::string& config_path, uint32_t target);    // full memory scan for HP value
void runScanPtr  (const std::string& config_path, uintptr_t target);  // find pointers to an address

// diag_map.cpp
void runMapMode(const std::string& config_path, uintptr_t override_pb);
