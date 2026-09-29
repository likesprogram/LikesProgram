#pragma once
#include <LikesProgram/Net/system/LikesProgramNetExport.hpp>
#include <cstddef>
#include <memory>

namespace LikesProgram {
    namespace Net {
        namespace Internal {
            class ConnectionPoolState;
        }

        class EventLoop;

        class LIKESPROGRAM_NET_API EventLoopGroup {
        public:
            // 创建固定数量的 worker EventLoop，workerCount 为 0 时保持空组。
            explicit EventLoopGroup(std::size_t workerCount);
            // 停止并回收所有 worker 线程。
            ~EventLoopGroup();

            EventLoopGroup(const EventLoopGroup&) = delete;
            EventLoopGroup& operator=(const EventLoopGroup&) = delete;

            // 启动所有 worker loop；重复调用不创建第二批线程。
            void Start();
            // 请求所有 worker loop 停止并等待线程退出。
            void Shutdown();
            // 轮询返回下一个 worker loop，空组返回 nullptr。
            EventLoop* NextLoop() noexcept;
            // 返回 worker loop 数量。
            std::size_t Size() const noexcept;
            // 返回 worker 组是否已经启动。
            bool IsRunning() const noexcept;

        private:
            friend class Internal::ConnectionPoolState;
            struct EventLoopGroupImpl;

            // 为连接池返回带生命周期所有权的 worker loop 快照。
            std::shared_ptr<EventLoop> NextLoopShared() noexcept;
            EventLoopGroupImpl* m_impl = nullptr; // 隐藏线程、EventLoop 容器和轮询状态
        };
    }
}
