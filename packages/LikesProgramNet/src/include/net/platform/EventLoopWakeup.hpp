#pragma once

// 各平台 EventLoop 唤醒实现共享的私有契约。
#include <LikesProgram/Net/SocketType.hpp>

namespace LikesProgram {
    namespace Net {
        namespace Internal {
            class EventLoopWakeup {
            public:
                // 创建可被 Poller 监听的内部唤醒句柄。
                EventLoopWakeup();
                // 关闭内部唤醒句柄，释放平台 socket 或 fd。
                ~EventLoopWakeup();

                EventLoopWakeup(const EventLoopWakeup&) = delete;
                EventLoopWakeup& operator=(const EventLoopWakeup&) = delete;

                // 返回唤醒句柄是否创建成功。
                bool IsValid() const noexcept;
                // 返回用于注册到 EventLoop Poller 的可读端。
                SocketType ReadSocket() const noexcept;
                // 写入一次唤醒信号，使阻塞中的 EventLoop 立即返回。
                bool Wakeup() noexcept;
                // 清空已到达的唤醒信号，避免重复触发空事件。
                void Drain() noexcept;

            private:
                SocketType m_readSocket = kInvalidSocket;  // Poller 监听的可读端。
                SocketType m_writeSocket = kInvalidSocket; // 跨线程写入的唤醒端。
            };
        }
    }
}
