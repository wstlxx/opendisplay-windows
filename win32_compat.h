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

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#endif  // _WIN32
