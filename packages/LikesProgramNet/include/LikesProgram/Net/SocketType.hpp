#pragma once

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 6101)
#endif
#include <winsock2.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#else
#include <sys/socket.h>
#endif

namespace LikesProgram {
    namespace Net {
#ifdef _WIN32
        using SocketType = SOCKET;
        using SocketLength = int;
        constexpr SocketType kInvalidSocket = INVALID_SOCKET;
#else
        using SocketType = int;
        using SocketLength = socklen_t;
        constexpr SocketType kInvalidSocket = -1;
#endif
    }
}
