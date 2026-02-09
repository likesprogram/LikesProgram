#include "../../../include/LikesProgram/net/Transport.hpp"
#include <cstdint>
#include <algorithm>
#if defined(_WIN32)
#include <ws2tcpip.h>
// Windows: errno 不适用于 socket，使用 WSAGetLastError
static int GetSockErr() { return ::WSAGetLastError(); }
static bool IsWouldBlock(int e) { return e == WSAEWOULDBLOCK; }
static bool IsInterrupted(int e) { return e == WSAEINTR; }
static void CloseSocket(SocketType fd) { ::closesocket(fd); }
static int ShutdownWriteSock(SocketType fd) { return ::shutdown(fd, SD_SEND); }
#else
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
static int GetSockErr() { return errno; }
static bool IsWouldBlock(int e) { return e == EAGAIN || e == EWOULDBLOCK; }
static bool IsInterrupted(int e) { return e == EINTR; }
static void CloseSocket(SocketType fd) { ::close(fd); }
static int ShutdownWriteSock(SocketType fd) { return ::shutdown(fd, SHUT_WR); }
#endif

namespace LikesProgram {
    namespace Net {
        static inline IoResult MakeOk(int64_t n) {
            return { IoStatus::Ok, n, 0 };
        }
        static inline IoResult MakeWouldBlock() {
            return { IoStatus::WouldBlock, 0, 0 };
        }
        static inline IoResult MakePeerClosed() {
            return { IoStatus::PeerClosed, 0, 0 };
        }
        static inline IoResult MakeError(int err) {
            return { IoStatus::Error, 0, err };
        }

        IoResult TcpTransport::ReadSome(Buffer& in) {
            if (m_closed.load(std::memory_order_acquire) || m_fd < 0) {
                return MakeError(/*err*/0);
            }

            // 循环读取直到 WouldBlock（适配 epoll ET；LT 也无害）
            int64_t total = 0;
            for (;;) {
                // 单次最多期望读入 64KB（可按需调整）
                constexpr size_t kMaxChunk = 64 * 1024;

                // 确保有可写空间
                in.EnsureWritableBytes(kMaxChunk);

                uint8_t* dst = in.BeginWrite();
                const size_t cap = std::min(in.WritableBytes(), kMaxChunk);

#if defined(_WIN32)
                int n = ::recv(m_fd, reinterpret_cast<char*>(dst), static_cast<int>(cap), 0);
#else
                ssize_t n = ::recv(m_fd, dst, cap, 0);
#endif
                if (n > 0) {
                    in.HasWritten(static_cast<size_t>(n));
                    total += static_cast<int64_t>(n);
                    continue; // ET：继续读
                }

                if (n == 0) {
                    // 对端 FIN，读到 EOF
                    if (total > 0) return MakeOk(total);
                    return MakePeerClosed();
                }

                // n < 0
                const int err = GetSockErr();
                if (IsInterrupted(err)) continue; // EINTR：重试

                if (IsWouldBlock(err)) {
                    // 非阻塞读完了
                    if (total > 0) return MakeOk(total);
                    return MakeWouldBlock();
                }

                // 真实错误
                if (total > 0) return MakeOk(total); // 已经有进展，先把进展交给上层处理
                return MakeError(err);
            }
        }

        IoResult TcpTransport::WriteSome(const uint8_t* p, size_t len) {
            if (m_closed.load(std::memory_order_acquire) || m_fd < 0) {
                return MakeError(/*err*/0);
            }

            if (p == nullptr || len == 0) {
                // 没有数据可写：这不算错误
                return MakeOk(0);
            }

            int64_t total = 0;

            // 尽力写（可写多少写多少），直到 WouldBlock / 写完
            while (len > 0) {
#if defined(_WIN32)
                const int chunk = (len > static_cast<size_t>(INT_MAX)) ? INT_MAX : static_cast<int>(len);
                int n = ::send(m_fd, reinterpret_cast<const char*>(p), chunk, 0);
#else
                ssize_t n = ::send(m_fd, p, len, 0);
#endif
                if (n > 0) {
                    total += static_cast<int64_t>(n);
                    p += n;
                    len -= static_cast<size_t>(n);
                    continue;
                }

                // n <= 0
                const int err = GetSockErr();
                if (IsInterrupted(err)) continue;

                if (IsWouldBlock(err)) {
                    if (total > 0) return MakeOk(total);
                    return MakeWouldBlock();
                }

                // 真实错误
                if (total > 0) return MakeOk(total);
                return MakeError(err);
            }

            return MakeOk(total);
        }

        void TcpTransport::ShutdownWrite() {
            if (m_fd == kInvalidSocket) return;
            // shutdown 失败不一定致命（比如已关闭），不强行标错
            (void)ShutdownWriteSock(m_fd);
        }

        void TcpTransport::Close() {
            if (m_closed.exchange(true, std::memory_order_acq_rel)) {
                return;
            }
            if (m_fd >= 0) {
                CloseSocket(m_fd);
                m_fd = (SocketType)-1;
            }
        }
    }
}
