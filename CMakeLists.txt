# OpenDisplay protocol 3 receiver for Windows (native C++20, Win32).
#
# Build (on Windows, VS 2022 / MSVC):
#   cmake -B build
#   cmake --build build --config Release
#   build\Release\opendisplay_receiver.exe
#
# The protocol module is platform-independent and is built + unit-tested on
# any platform (CI / Linux):
#   cmake -B build && cmake --build build && ./build/od_tests

cmake_minimum_required(VERSION 3.20)
project(opendisplay_receiver CXX)

enable_testing()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Static CRT for every target: keeps the final exe self-contained (no VC
# redist needed) and avoids /MT-vs-/MD mismatches between our static libs and
# the exe that would leave CRT symbols (__imp_fopen, ...) unresolved.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# ---------------------------------------------------------------------------
# Protocol module: pure C++, no platform dependencies.
# ---------------------------------------------------------------------------
add_library(od_protocol STATIC
    protocol/framing.cpp
    protocol/annexb.cpp
    protocol/sps.cpp
    protocol/control.cpp
    protocol/demux.cpp
)
target_include_directories(od_protocol PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# ---------------------------------------------------------------------------
# Unit tests (run anywhere, including Linux/macOS).
# ---------------------------------------------------------------------------
add_executable(od_tests tests/test_protocol.cpp app/config.cpp)
target_link_libraries(od_tests PRIVATE od_protocol)
add_test(NAME protocol COMMAND od_tests)

if(MSVC)
    target_compile_options(od_protocol PUBLIC /W4 /permissive-)
    target_compile_options(od_tests PRIVATE /W4 /permissive-)
else()
    target_compile_options(od_protocol PUBLIC -Wall -Wextra -Wpedantic)
    target_compile_options(od_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# The receiver application (Windows only).
# ---------------------------------------------------------------------------
if(WIN32)
    # Wide-char Win32: our window code uses L"..." literals, so select the W
    # variants of the Win32 string APIs (CreateWindowExW, RegisterClassExW, ...).
    add_compile_definitions(UNICODE _UNICODE)

    add_library(od_net STATIC
        net/log.cpp
        net/tcp_listener.cpp
        net/session.cpp
        net/mdns_advertise.cpp
    )
    target_link_libraries(od_net PUBLIC od_protocol ws2_32 iphlpapi)

    add_library(od_video STATIC
        video/h264_decoder.cpp
    )
    target_link_libraries(od_video
        PUBLIC od_protocol mfplat mf mfuuid mfreadwrite d3d11 dxgi)

    add_library(od_render STATIC
        render/renderer.cpp
    )
    target_link_libraries(od_render PUBLIC od_video d3d11 dxgi d3dcompiler)

    add_executable(opendisplay_receiver app/main.cpp app/config.cpp)
    target_link_libraries(opendisplay_receiver
        PRIVATE od_render od_video od_net user32 gdi32 ole32
    )
    if(MSVC)
        target_compile_options(opendisplay_receiver PRIVATE /W4 /permissive-)
    endif()
endif()
