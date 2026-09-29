#include "net/platform/SocketOps.hpp"

#if defined(__linux__)

#include <algorithm>
#include <cerrno>
#include <limits>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace LikesProgram {
    namespace Net {
        namespace Internal {
            SocketRuntime::SocketRuntime() = default;
            SocketRuntime::~SocketRuntime() = default;

            void SocketRuntime::Ensure() {
                static SocketRuntime runtime; // 保持各平台一致的进程级初始化入口
                (void)runtime;
            }

            int GetLastSocketError() noexcept {
                return errno;
            }

            bool IsWouldBlock(int error) noexcept {
                return error == EAGAIN || error == EWOULDBLOCK || error == EINPROGRESS;
            }

            bool IsInterrupted(int error) noexcept {
                return error == EINTR;
            }

            void CloseSocket(SocketType fd) noexcept {
                if (fd != kInvalidSocket) (void)::close(fd);
            }

            int ShutdownWrite(SocketType fd) noexcept {
                return fd == kInvalidSocket ? -1 : ::shutdown(fd, SHUT_WR);
            }

            bool SetNonBlocking(SocketType fd, bool enabled) noexcept {
                if (fd == kInvalidSocket) return false;

                const int flags = ::fcntl(fd, F_GETFL, 0); // 保留现有 fd 属性
                if (flags < 0) return false;
                const int nextFlags = enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
                return ::fcntl(fd, F_SETFL, nextFlags) == 0;
            }

            int GetSocketPendingError(SocketType fd) noexcept {
                int error = 0; // SO_ERROR 返回的异步连接错误
                SocketLength length = static_cast<SocketLength>(sizeof(error));
                if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) != 0) return errno;
                return error;
            }

            SocketType CreateSocket(int family, int type, int protocol) noexcept {
                SocketRuntime::Ensure();
                return ::socket(family, type, protocol);
            }

            int ConnectSocket(SocketType fd, const sockaddr* address, SocketLength length) noexcept {
                // 连接结果保持 Linux errno 语义，由上层统一解释 EINPROGRESS。
                return ::connect(fd, address, length);
            }

            int BindSocket(SocketType fd, const sockaddr* address, SocketLength length) noexcept {
                // 绑定结果保持平台原始返回值，避免通用层依赖系统头。
                return ::bind(fd, address, length);
            }

            int ListenSocket(SocketType fd, int backlog) noexcept {
                // 监听只属于平台 socket 边界，通用 Server 不直接调用系统 API。
                return ::listen(fd, backlog);
            }

            SocketType AcceptSocket(SocketType fd, sockaddr* peer, SocketLength* peerLength) noexcept {
                // accept4 一次性设置非阻塞与 close-on-exec，避免 fd 交付前出现窗口。
                return ::accept4(fd, peer, peerLength, SOCK_NONBLOCK | SOCK_CLOEXEC);
            }

            bool SupportsWorkerLocalListenerReuse() noexcept {
                // Linux 当前 completion listener 依赖 SO_REUSEPORT 分发新连接。
                return true;
            }

            bool SetReuseAddress(SocketType fd) noexcept {
                int enabled = 1; // 允许服务快速复用监听地址
                return ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled,
                    static_cast<SocketLength>(sizeof(enabled))) == 0;
            }

            bool SetReusePort(SocketType fd) noexcept {
                int enabled = 1; // 内核按 flow hash 把新连接分发到 worker-local listener
                return ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &enabled,
                    static_cast<SocketLength>(sizeof(enabled))) == 0;
            }

            bool SetTcpNoDelay(SocketType fd) noexcept {
                int enabled = 1; // 低延迟 TCP 默认关闭 Nagle
                return ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled,
                    static_cast<SocketLength>(sizeof(enabled))) == 0;
            }

            int InvalidSocketArgumentError() noexcept {
                return EINVAL;
            }

            std::int64_t ReceiveSocket(
                SocketType fd,
                void* data,
                std::size_t len,
                int flags) noexcept {
                const std::size_t safeLength = (std::min)(
                    len,
                    static_cast<std::size_t>((std::numeric_limits<ssize_t>::max)())); // 防止长度转换越界
                return static_cast<std::int64_t>(::recv(fd, data, safeLength, flags));
            }

            std::int64_t SendSocket(
                SocketType fd,
                const void* data,
                std::size_t len,
                int flags) noexcept {
                const std::size_t safeLength = (std::min)(
                    len,
                    static_cast<std::size_t>((std::numeric_limits<ssize_t>::max)())); // 保持单次系统调用长度可表示
                return static_cast<std::int64_t>(
                    ::send(fd, data, safeLength, flags | MSG_NOSIGNAL)); // 对端关闭只返回 EPIPE，不终止服务进程
            }

            std::int64_t ReceiveSocketFrom(
                SocketType fd,
                void* data,
                std::size_t len,
                int flags,
                sockaddr* peer,
                SocketLength* peerLength) noexcept {
                const std::size_t safeLength = (std::min)(
                    len,
                    static_cast<std::size_t>((std::numeric_limits<ssize_t>::max)())); // 保持 POSIX 长度有界
                return static_cast<std::int64_t>(
                    ::recvfrom(fd, data, safeLength, flags, peer, peerLength));
            }

            std::int64_t SendSocketTo(
                SocketType fd,
                const void* data,
                std::size_t len,
                int flags,
                const sockaddr* peer,
                SocketLength peerLength) noexcept {
                const std::size_t safeLength = (std::min)(
                    len,
                    static_cast<std::size_t>((std::numeric_limits<ssize_t>::max)())); // 保持 POSIX 长度有界
                return static_cast<std::int64_t>(
                    ::sendto(
                        fd,
                        data,
                        safeLength,
                        flags | MSG_NOSIGNAL,
                        peer,
                        peerLength)); // 统一抑制 Linux socket 写入 SIGPIPE
            }
        }
    }
}

#endif
