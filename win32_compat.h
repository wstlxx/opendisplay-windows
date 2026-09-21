// Single place where the platform headers are pulled in.
// Order matters: winsock2.h must precede windows.h.

#pragma once

#ifdef _WIN32

// We deliberately do NOT define WIN32_LEAN_AND_MEAN. Including winsock2.h
// before windows.h is enough to avoid the winsock.h/winsock2.h conflict, and
// the Media Foundation / D3D headers expect the full windows.h to be present.
#ifndef NOMINMAX
#define NOMINMAX
#endif

// Pin the target OS version ONCE, before any D3D/DXGI/MF header. Without this,
// the value those headers see can depend on include order (e.g. an MF header
// included before d3d11.h in one TU but after it in another), which can skew
// conditional type layouts and cause cross-TU ABI mismatches. Target Win10.
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#endif  // _WIN32
