cmake_minimum_required(VERSION 3.16)
project(rdga1bot CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenCV REQUIRED)
find_package(Threads REQUIRED)

# ── Platform-agnostic sources ─────────────────────────────────────────────────
set(SOURCES
    src/main.cpp
    src/Brain.cpp
    src/BotBehaviorTree.cpp
    src/BehaviorTree.cpp
    src/Config.cpp
    src/Stats.cpp
    src/Notify.cpp
    src/Eyes.cpp
    src/Utils.cpp
    src/Input.cpp
    src/Dashboard.cpp
    src/MemReader.cpp
    src/offset_scanner.cpp
    src/knownlist_reader.cpp
    src/world_state.cpp
    src/Geodata.cpp
    src/vision_worker.cpp
    src/geodata_worker.cpp
    src/navmesh_builder.cpp
    src/navmesh_worker.cpp
    src/LinearQModel.cpp
    src/LearningWorker.cpp
    src/ShadowLogger.cpp
    src/MemoryValidator.cpp
)

add_executable(rdga1bot ${SOURCES})

target_include_directories(rdga1bot PRIVATE src ${OpenCV_INCLUDE_DIRS})

# ── Platform-specific sources and libraries ───────────────────────────────────
if(WIN32)
    target_sources(rdga1bot PRIVATE
        src/Intercept.cpp
        src/Capture.cpp
        src/Window.cpp
    )

    # PDCurses — drop-in ncurses replacement (set PDCURSES_DIR env var)
    if(DEFINED ENV{PDCURSES_DIR})
        target_include_directories(rdga1bot PRIVATE $ENV{PDCURSES_DIR})
        target_link_libraries(rdga1bot PRIVATE $ENV{PDCURSES_DIR}/pdcurses.lib)
    else()
        find_library(PDCURSES_LIB NAMES pdcurses pdcurses_static)
        if(PDCURSES_LIB)
            target_link_libraries(rdga1bot PRIVATE ${PDCURSES_LIB})
        else()
            message(WARNING "PDCurses not found — set PDCURSES_DIR for TUI support")
            target_compile_definitions(rdga1bot PRIVATE RDGA1BOT_NO_TUI)
        endif()
    endif()

    # interception.dll kernel-mode driver (set INTERCEPTION_DIR env var)
    if(DEFINED ENV{INTERCEPTION_DIR})
        target_include_directories(rdga1bot PRIVATE $ENV{INTERCEPTION_DIR}/include)
        target_link_libraries(rdga1bot PRIVATE $ENV{INTERCEPTION_DIR}/lib/interception.lib)
    else()
        message(FATAL_ERROR "INTERCEPTION_DIR not set — required for Windows input backend.\n"
                            "Install interception driver and set INTERCEPTION_DIR.")
    endif()

    target_link_libraries(rdga1bot PRIVATE psapi)
    target_compile_definitions(rdga1bot PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(rdga1bot PRIVATE /W3 /O2)

else()
    target_sources(rdga1bot PRIVATE
        src/Intercept_Linux.cpp
        src/Capture_Linux.cpp
        src/Window_Linux.cpp
    )

    target_link_libraries(rdga1bot PRIVATE X11 Xtst Xext ncursesw)
    target_compile_options(rdga1bot PRIVATE -Wall -O2)
endif()

target_link_libraries(rdga1bot PRIVATE ${OpenCV_LIBS} Threads::Threads)

install(TARGETS rdga1bot DESTINATION .)
