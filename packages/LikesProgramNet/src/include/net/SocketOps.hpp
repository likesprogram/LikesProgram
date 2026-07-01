#pragma once
#include <LikesProgram/Net/SocketType.hpp>
#include <cstdint>

#ifdef _WIN32
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 6101)
#endif
#include <ws2tcpip.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#else
#include <cerrno>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <unistd.h>
#endif

namespace LikesProgram {
    namespace Net {
        namespace Internal {
            class SocketRuntime {
            public:
                // 确保当前进程已完成平台 socket 运行时初始化。
                static void Ensure();

            private:
                // 构造时执行平台初始化。
                SocketRuntime();
                // 析构时释放平台运行时资源。
                ~SocketRuntime();
            };

            // 返回最近一次 socket 错误码。
            int GetLastSocketError() noexcept;
            // 判断错误码是否表示非阻塞暂不可读写。
            bool IsWouldBlock(int error) noexcept;
            // 判断错误码是否表示系统调用被信号中断。
            bool IsInterrupted(int error) noexcept;
            // 关闭 socket，忽略无效句柄。
            void CloseSocket(SocketType fd) noexcept;
            // 关闭 socket 写端。
            int ShutdownWrite(SocketType fd) noexcept;
            // 设置或取消非阻塞模式。
            bool SetNonBlocking(SocketType fd, bool enabled = true) noexcept;
            // 读取非阻塞 connect 的 SO_ERROR。
            int GetSocketPendingError(SocketType fd) noexcept;
            // 创建 socket 并保证运行时初始化已完成。
            SocketType CreateSocket(int family, int type, int protocol) noexcept;
            // 设置 SO_REUSEADDR，失败时返回 false。
            bool SetReuseAddress(SocketType fd) noexcept;
            // 设置 TCP_NODELAY，失败时返回 false。
            bool SetTcpNoDelay(SocketType fd) noexcept;
        }
    }
}
