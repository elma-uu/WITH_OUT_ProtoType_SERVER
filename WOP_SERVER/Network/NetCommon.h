#pragma once

// Registered I/O (RIO) requires Windows 8 (0x0602) or later.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>

#pragma comment(lib, "ws2_32.lib")

#include <cstdint>
#include <cstdio>

namespace Wop
{
    constexpr ULONG_PTR kRecvCompletionKey = 1;
    constexpr ULONG_PTR kSendCompletionKey = 2;
    constexpr ULONG_PTR kShutdownCompletionKey = 9999;

    inline PVOID const kRecvRequestTag = reinterpret_cast<PVOID>(static_cast<ULONG_PTR>(1));
    inline PVOID const kSendRequestTag = reinterpret_cast<PVOID>(static_cast<ULONG_PTR>(2));
}
