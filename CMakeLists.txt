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

# Pin the Windows SDK for the Visual Studio generator. On GitHub's windows
# runners the newest installed SDK (10.0.26100.0) ships a broken mfreadwrite.h
# (a MIDL interface stub that lacks IMFMediaSourceReader / MFCreateMediaSource
# Reader / MFStreamStatus / MF_SEEK_ORIGIN / MF_DECODE_TO_DISPLAY / ...), which
# makes any Media Foundation decode code fail to compile. 10.0.22621.0 (the
# Windows 11 SDK) has a healthy Media Foundation SDK. Only affects the VS
# generator; ignored on Linux. Verified by the od_mf_diag target (video/mf_diag.cpp).
# The authoritative pin is passed on the command line in CI as
# -DCMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION=10.0.19041.0 (a plain set() here
# is ignored by the VS generator's SDK auto-selection).
set(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION "10.0.19041.0")

project(opendisplay_receiver CXX)

enable_testing()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
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
add_executable(od_tests tests/test_protocol.cpp)
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
    add_library(od_net STATIC
        net/log.cpp
        net/tcp_listener.cpp
        net/session.cpp
    )
    target_link_libraries(od_net PUBLIC od_protocol ws2_32)

    add_library(od_video STATIC
        video/h264_decoder.cpp
    )
    target_link_libraries(od_video
        PUBLIC od_protocol mfplat mf mfuuid mfidl d3d11 dxgi)

    add_library(od_render STATIC
        render/renderer.cpp
    )
    target_link_libraries(od_render PUBLIC od_video d3d11 dxgi d3dcompiler)

    # CI diagnostic: verifies the MF/D3D SDK headers resolve on the runner.
    add_executable(od_mf_diag video/mf_diag.cpp)
    target_link_libraries(od_mf_diag PRIVATE mfplat mf mfuuid mfidl d3d11 dxgi)

    add_executable(opendisplay_receiver app/main.cpp)
    target_link_libraries(opendisplay_receiver
        PRIVATE od_render od_video od_net user32 gdi32 ole32
    )
    if(MSVC)
        target_compile_options(opendisplay_receiver PRIVATE /W4 /permissive-)
        # Static CRT so the exe runs on machines without the VC redist.
        set_property(TARGET opendisplay_receiver PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endif()
