#include "net/SocketOps.hpp"

#ifdef _WIN32
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
        namespace Internal {
            SocketRuntime::SocketRuntime() {
#ifdef _WIN32
                WSADATA data{}; // Winsock 启动结果，当前只需确保调用成功
                (void)::WSAStartup(MAKEWORD(2, 2), &data);
#endif
            }

            SocketRuntime::~SocketRuntime() {
#ifdef _WIN32
                (void)::WSACleanup();
#endif
            }

            void SocketRuntime::Ensure() {
                static SocketRuntime runtime; // 进程级 socket 运行时，C++ 保证线程安全初始化
                (void)runtime;
            }

            int GetLastSocketError() noexcept {
#ifdef _WIN32
                return ::WSAGetLastError();
#else
                return errno;
#endif
            }

            bool IsWouldBlock(int error) noexcept {
#ifdef _WIN32
                return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
#else
                return error == EAGAIN || error == EWOULDBLOCK || error == EINPROGRESS;
#endif
            }

            bool IsInterrupted(int error) noexcept {
#ifdef _WIN32
                return error == WSAEINTR;
#else
                return error == EINTR;
#endif
            }

            void CloseSocket(SocketType fd) noexcept {
                if (fd == kInvalidSocket) return;
#ifdef _WIN32
                (void)::closesocket(fd);
#else
                (void)::close(fd);
#endif
            }

            int ShutdownWrite(SocketType fd) noexcept {
                if (fd == kInvalidSocket) return -1;
#ifdef _WIN32
                return ::shutdown(fd, SD_SEND);
#else
                return ::shutdown(fd, SHUT_WR);
#endif
            }

            bool SetNonBlocking(SocketType fd, bool enabled) noexcept {
                if (fd == kInvalidSocket) return false;
#ifdef _WIN32
                u_long mode = enabled ? 1UL : 0UL; // Winsock 用 0/1 切换阻塞模式
                return ::ioctlsocket(fd, FIONBIO, &mode) == 0;
#else
                const int flags = ::fcntl(fd, F_GETFL, 0); // 读取现有 flag，避免覆盖其他属性
                if (flags < 0) return false;
                const int nextFlags = enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
                return ::fcntl(fd, F_SETFL, nextFlags) == 0;
#endif
            }

            int GetSocketPendingError(SocketType fd) noexcept {
                int error = 0; // SO_ERROR 的系统错误码
                SocketLength length = static_cast<SocketLength>(sizeof(error));
                if (::getsockopt(fd, SOL_SOCKET, SO_ERROR,
#ifdef _WIN32
                    reinterpret_cast<char*>(&error),
#else
                    &error,
#endif
                    &length) != 0) {
                    return GetLastSocketError();
                }

                return error;
            }

            SocketType CreateSocket(int family, int type, int protocol) noexcept {
                SocketRuntime::Ensure();
                return ::socket(family, type, protocol);
            }

            bool SetReuseAddress(SocketType fd) noexcept {
                int enabled = 1; // 允许测试和短生命周期服务快速复用端口
                return ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
#ifdef _WIN32
                    reinterpret_cast<const char*>(&enabled),
#else
                    &enabled,
#endif
                    static_cast<SocketLength>(sizeof(enabled))) == 0;
            }

            bool SetTcpNoDelay(SocketType fd) noexcept {
                int enabled = 1; // 低延迟协议默认关闭 Nagle
                return ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
#ifdef _WIN32
                    reinterpret_cast<const char*>(&enabled),
#else
                    &enabled,
#endif
                    static_cast<SocketLength>(sizeof(enabled))) == 0;
            }
        }
    }
}
