cmake_minimum_required(VERSION 3.16)
project(rdga1bot CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenCV REQUIRED)

# Threads для std::thread (Notify)
find_package(Threads REQUIRED)

set(SOURCES
    src/main.cpp
    src/Brain.cpp
    src/Config.cpp
    src/Stats.cpp
    src/Notify.cpp
    src/Eyes.cpp
    src/Utils.cpp
    src/Input.cpp
    src/Capture.cpp
    src/Capture_Linux.cpp
    src/Intercept.cpp
    src/Intercept_Linux.cpp
    src/Window.cpp
    src/Window_Linux.cpp
    src/Dashboard.cpp
    src/MemReader.cpp
    src/offset_scanner.cpp
    src/knownlist_reader.cpp
    src/world_state.cpp
    src/Geodata.cpp
)

add_executable(rdga1bot ${SOURCES})

target_include_directories(rdga1bot PRIVATE src)

target_link_libraries(rdga1bot
    ${OpenCV_LIBS}
    Threads::Threads
    X11
    Xtst
    Xext
    ncursesw
)

target_compile_options(rdga1bot PRIVATE -Wall -O2)

install(TARGETS rdga1bot DESTINATION .)
