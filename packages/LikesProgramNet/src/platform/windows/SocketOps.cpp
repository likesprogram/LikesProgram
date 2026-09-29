#include "net/platform/SocketOps.hpp"

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace LikesProgram {
    namespace Net {
        namespace Internal {
            SocketRuntime::SocketRuntime() {
                WSADATA data{}; // Winsock 2.2 初始化结果
                if (::WSAStartup(MAKEWORD(2, 2), &data) != 0) {
                    throw std::runtime_error("WSAStartup failed");
                }
            }

            SocketRuntime::~SocketRuntime() {
                (void)::WSACleanup();
            }

            void SocketRuntime::Ensure() {
                static SocketRuntime runtime; // 进程级 Winsock 生命周期
                (void)runtime;
            }

            int GetLastSocketError() noexcept {
                return ::WSAGetLastError();
            }

            bool IsWouldBlock(int error) noexcept {
                return error == WSAEWOULDBLOCK
                    || error == WSAEINPROGRESS
                    || error == WSA_IO_PENDING;
            }

            bool IsInterrupted(int error) noexcept {
                return error == WSAEINTR;
            }

            void CloseSocket(SocketType fd) noexcept {
                if (fd != kInvalidSocket) (void)::closesocket(fd);
            }

            int ShutdownWrite(SocketType fd) noexcept {
                return fd == kInvalidSocket ? SOCKET_ERROR : ::shutdown(fd, SD_SEND);
            }

            bool SetNonBlocking(SocketType fd, bool enabled) noexcept {
                if (fd == kInvalidSocket) return false;
                u_long value = enabled ? 1UL : 0UL; // FIONBIO 的开关值
                return ::ioctlsocket(fd, FIONBIO, &value) == 0;
            }

            int GetSocketPendingError(SocketType fd) noexcept {
                int error = 0; // SO_ERROR 返回的异步连接错误
                SocketLength length = static_cast<SocketLength>(sizeof(error));
                if (::getsockopt(
                    fd,
                    SOL_SOCKET,
                    SO_ERROR,
                    reinterpret_cast<char*>(&error),
                    &length) != 0) {
                    return GetLastSocketError();
                }
                return error;
            }

            SocketType CreateSocket(int family, int type, int protocol) noexcept {
                SocketRuntime::Ensure();
                return ::WSASocketW(
                    family,
                    type,
                    protocol,
                    nullptr,
                    0,
                    WSA_FLAG_OVERLAPPED);
            }

            int ConnectSocket(SocketType fd, const sockaddr* address, SocketLength length) noexcept {
                return ::connect(fd, address, length);
            }

            int BindSocket(SocketType fd, const sockaddr* address, SocketLength length) noexcept {
                return ::bind(fd, address, length);
            }

            int ListenSocket(SocketType fd, int backlog) noexcept {
                return ::listen(fd, backlog);
            }

            SocketType AcceptSocket(SocketType fd, sockaddr* peer, SocketLength* peerLength) noexcept {
                const SocketType accepted = ::accept(fd, peer, peerLength);
                if (accepted == kInvalidSocket) return kInvalidSocket;
                if (!SetNonBlocking(accepted, true)) {
                    CloseSocket(accepted);
                    return kInvalidSocket;
                }
                return accepted;
            }

            bool SupportsWorkerLocalListenerReuse() noexcept {
                return false; // IOCP 首版由单一 accept issuer 负责 listener
            }

            bool SetReuseAddress(SocketType fd) noexcept {
                int enabled = 1; // 允许服务快速复用监听地址
                return ::setsockopt(
                    fd,
                    SOL_SOCKET,
                    SO_REUSEADDR,
                    reinterpret_cast<const char*>(&enabled),
                    static_cast<SocketLength>(sizeof(enabled))) == 0;
            }

            bool SetReusePort(SocketType) noexcept {
                return false; // Winsock 没有 POSIX SO_REUSEPORT 对等语义
            }

            bool SetTcpNoDelay(SocketType fd) noexcept {
                int enabled = 1; // 低延迟 TCP 默认关闭 Nagle
                return ::setsockopt(
                    fd,
                    IPPROTO_TCP,
                    TCP_NODELAY,
                    reinterpret_cast<const char*>(&enabled),
                    static_cast<SocketLength>(sizeof(enabled))) == 0;
            }

            int InvalidSocketArgumentError() noexcept {
                return WSAEINVAL;
            }

            std::int64_t ReceiveSocket(
                SocketType fd,
                void* data,
                std::size_t len,
                int flags) noexcept {
                const int safeLength = static_cast<int>((std::min)(
                    len,
                    static_cast<std::size_t>((std::numeric_limits<int>::max)())));
                return static_cast<std::int64_t>(
                    ::recv(fd, static_cast<char*>(data), safeLength, flags));
            }

            std::int64_t SendSocket(
                SocketType fd,
                const void* data,
                std::size_t len,
                int flags) noexcept {
                const int safeLength = static_cast<int>((std::min)(
                    len,
                    static_cast<std::size_t>((std::numeric_limits<int>::max)())));
                return static_cast<std::int64_t>(
                    ::send(fd, static_cast<const char*>(data), safeLength, flags));
            }

            std::int64_t ReceiveSocketFrom(
                SocketType fd,
                void* data,
                std::size_t len,
                int flags,
                sockaddr* peer,
                SocketLength* peerLength) noexcept {
                const int safeLength = static_cast<int>((std::min)(
                    len,
                    static_cast<std::size_t>((std::numeric_limits<int>::max)())));
                return static_cast<std::int64_t>(::recvfrom(
                    fd,
                    static_cast<char*>(data),
                    safeLength,
                    flags,
                    peer,
                    peerLength));
            }

            std::int64_t SendSocketTo(
                SocketType fd,
                const void* data,
                std::size_t len,
                int flags,
                const sockaddr* peer,
                SocketLength peerLength) noexcept {
                const int safeLength = static_cast<int>((std::min)(
                    len,
                    static_cast<std::size_t>((std::numeric_limits<int>::max)())));
                return static_cast<std::int64_t>(::sendto(
                    fd,
                    static_cast<const char*>(data),
                    safeLength,
                    flags,
                    peer,
                    peerLength));
            }
        }
    }
}

#endif
