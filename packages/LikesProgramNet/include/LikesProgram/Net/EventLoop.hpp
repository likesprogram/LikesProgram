#pragma once
#include <LikesProgram/Net/system/LikesProgramNetExport.hpp>
#include <LikesProgram/Net/Poller.hpp>
#include <functional>
#include <memory>
#include <vector>

namespace LikesProgram {
    namespace Net {
        class Connection;

        class LIKESPROGRAM_NET_API EventLoop {
        public:
            using Task = std::function<void()>;

            // 使用平台默认 Poller 创建事件循环。
            EventLoop();
            // 使用调用方提供的 Poller 创建事件循环。
            explicit EventLoop(std::unique_ptr<Poller> poller);
            virtual ~EventLoop();

            EventLoop(const EventLoop&) = delete;
            EventLoop& operator=(const EventLoop&) = delete;

            // 在当前线程启动事件循环，直到 Shutdown 被调用。
            void Start();
            // 请求事件循环停止，跨线程调用会在短轮询超时后生效。
            void Shutdown();
            // 返回当前线程是否为事件循环线程。
            bool IsInLoopThread() const noexcept;
            // 注册一个 Channel。
            bool RegisterChannel(Channel* channel);
            // 注销一个 Channel。
            bool UnregisterChannel(Channel* channel);
            // 更新一个 Channel 的关注事件。
            bool UpdateChannel(Channel* channel);
            // 投递一个任务到事件循环线程顺序执行。
            void PostTask(Task task);
            // 由 Server/Client 持有连接，避免 Channel 回调悬空。
            void AttachConnection(const std::shared_ptr<Connection>& connection);
            // 移除指定 socket 对应的连接持有，并返回移除前快照。
            std::shared_ptr<Connection> DetachConnection(SocketType fd);
            // 设置轮询超时，单位毫秒。
            void SetPollTimeout(int timeoutMs) noexcept;

        protected:
            // 处理 Poller 返回的活跃 Channel。
            virtual void ProcessEvents(const std::vector<Channel*>& activeChannels);
            // 执行待处理任务队列。
            void ProcessPendingTasks();
            // 返回底层 Poller 引用，仅供子类在 loop 线程内使用。
            Poller& PollerRef();

        private:
            struct EventLoopImpl;

            EventLoopImpl* m_impl = nullptr;                          // 事件循环实现，隐藏线程与 STL 状态
        };
    }
}
