#pragma once
#include "Poller.hpp"                  // Poller 接口，用于事件轮询
#include "Channel.hpp"                 // Channel 封装 fd + 回调
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

namespace LikesProgram {
    namespace Net {
        class Server;
        class EventLoop: public std::enable_shared_from_this<EventLoop> {
        public:
            // 创建 Poller 的工厂：每个 loop 必须独占一个 Poller
            using PollerFactory = std::function<std::unique_ptr<Poller>()>;
            using ConnectionFactory = std::function<std::shared_ptr<Connection>(SocketType, EventLoop*)>;

            using Task = std::function<void()>;
            explicit EventLoop(std::unique_ptr<Poller> poller);
            virtual ~EventLoop();

            EventLoop(const EventLoop&) = delete;
            EventLoop& operator=(const EventLoop&) = delete;

            // 启动
            void Run();
            // 停止
            void Stop();

            // 当前线程是否为 loop 线程
            bool IsInLoopThread() const noexcept;

            // 注册一个 Channel
            bool RegisterChannel(Channel* channel);

            // 注销一个 Channel
            bool UnregisterChannel(Channel* channel);

            // 更新一个 Channel（修改关注的事件）
            bool UpdateChannel(Channel* channel);

            // 提交任务到 loop 线程顺序执行（线程安全）
            void PostTask(Task task);

            // 连接生命周期持有（内部用，但需要 MainEventLoop 调用）
            // 约定：连接应在其归属 loop 线程 attach/detach（跨线程会自动 PostTask）
            void AttachConnection(const std::shared_ptr<Connection>& c);
            void DetachConnection(SocketType fd);

            // 对此 Loop 广播
            void BroadcastLocalExcept(const void* data, size_t len, const std::vector<SocketType>& removeSockets);
        protected:
            // 处理 Poller 返回的活跃 Channel
            // 子类（MainEventLoop）可 override 来做 accept 分发
            virtual void ProcessEvents(const std::vector<Channel*>& activeChannels);

            // 执行 pending tasks
            void ProcessPendingTasks();

            // 提供给子类：访问 poller 只在 loop 线程用
            Poller& PollerRef() { return *m_poller; }
        private:
            void InitWakeup();
            void Wakeup();                 // 唤醒 loop
            void HandleWakeupRead();
            void SetLoopThreadIdOnce();    // 在 Run() 内初始化

            std::unique_ptr<Poller> m_poller;                     // 轮询器指针
            std::atomic<bool> m_running = false;                 // 是否运行标志

            // loop 线程 id（Run 之后有效）
            std::atomic<bool> m_threadIdSet = false;
            std::thread::id m_loopThreadId{};

            // 连接持有：避免 Channel 中 Connection* 悬空
            std::unordered_map<SocketType, std::shared_ptr<Connection>> m_connections;
            std::mutex m_connMutex;

            // 任务队列
            std::vector<Task> m_pendingTasks;
            std::mutex m_tasksMutex;

            // wakeup（POSIX self-pipe）
            bool m_hasWakeup = false;
            SocketType m_wakeupReadFd;
            SocketType m_wakeupWriteFd;
            std::unique_ptr<Channel> m_wakeupChannel;

#ifdef _WIN32
            SocketType m_wakeupSock = kInvalidSocket;
            sockaddr_in m_wakeupAddr{};
#endif
            std::atomic_bool m_processingTasks = false;

            // poll 超时（毫秒）
            int m_pollTimeoutMs = 10;
        };
    }
}
