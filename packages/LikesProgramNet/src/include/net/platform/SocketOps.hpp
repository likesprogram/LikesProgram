#pragma once
#include <LikesProgram/Net/SocketType.hpp>
#include <cstddef>
#include <cstdint>

// 平台 socket 操作共享声明，系统头只进入对应平台实现。

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
            // 连接 socket 到远端地址，返回平台原始结果。
            int ConnectSocket(SocketType fd, const sockaddr* address, SocketLength length) noexcept;
            // 绑定 socket 到本地地址，返回平台原始结果。
            int BindSocket(SocketType fd, const sockaddr* address, SocketLength length) noexcept;
            // 启动 stream listener，返回平台原始结果。
            int ListenSocket(SocketType fd, int backlog) noexcept;
            // 接受一个连接并保证返回 socket 为 nonblocking/close-on-exec。
            SocketType AcceptSocket(SocketType fd, sockaddr* peer, SocketLength* peerLength) noexcept;
            // 返回当前平台能否安全创建 worker-local 端口复用 listener。
            bool SupportsWorkerLocalListenerReuse() noexcept;
            // 设置 SO_REUSEADDR，失败时返回 false。
            bool SetReuseAddress(SocketType fd) noexcept;
            // 设置 SO_REUSEPORT，供 worker-local listener 共享监听地址。
            bool SetReusePort(SocketType fd) noexcept;
            // 设置 TCP_NODELAY，失败时返回 false。
            bool SetTcpNoDelay(SocketType fd) noexcept;
            // 返回当前平台的无效参数错误码。
            int InvalidSocketArgumentError() noexcept;
            // 从 socket 接收字节，统一平台缓冲区指针与长度类型。
            std::int64_t ReceiveSocket(
                SocketType fd,
                void* data,
                std::size_t len,
                int flags) noexcept;
            // 向 socket 发送字节，统一平台缓冲区指针与长度类型。
            std::int64_t SendSocket(
                SocketType fd,
                const void* data,
                std::size_t len,
                int flags) noexcept;
            // 从 UDP socket 接收一个数据报并返回 peer。
            std::int64_t ReceiveSocketFrom(
                SocketType fd,
                void* data,
                std::size_t len,
                int flags,
                sockaddr* peer,
                SocketLength* peerLength) noexcept;
            // 向指定 UDP peer 发送一个数据报。
            std::int64_t SendSocketTo(
                SocketType fd,
                const void* data,
                std::size_t len,
                int flags,
                const sockaddr* peer,
                SocketLength peerLength) noexcept;
        }
    }
}
