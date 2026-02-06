#pragma once
#include "Poller.hpp"                  // Poller 接口，用于事件轮询
#include "Channel.hpp"                 // Channel 封装 fd + 回调
#include "Connection.hpp"
#include "../threading/ThreadPool.hpp" // 可选线程池
#include <unordered_set>
#include <memory>
#include <functional>
#include <atomic>
#include <mutex>

namespace LikesProgram {
    namespace Server {
        class EventLoop {
        public:
            explicit EventLoop(Poller* poller);

            ~EventLoop();

            // 设置指向主线程池的指针
            void SetMainThreadPool(std::shared_ptr<LikesProgram::ThreadPool> pool);

            // 注册一个 Channel
            bool RegisterChannel(Channel* channel);

            // 注销一个 Channel
            bool UnregisterChannel(Channel* channel);

            // 更新一个 Channel（修改关注的事件）
            bool UpdateChannel(Channel* channel);

            // 启动事件循环
            void Run();

            // 停止事件循环
            void Stop() {
                m_running.store(false, std::memory_order_relaxed); // 设置运行标志为 false
            }

            // 提交轻量任务到 EventLoop 内部顺序执行
            void PostTask(const std::function<void()>& task);

            // 提交耗时任务到 MainEventLoop 的线程池
            void PostHeavyTask(const std::function<void()>& task);

        protected:
            // 处理 Poller 返回的活跃 Channel
            virtual void ProcessEvents(const std::vector<Channel*>& activeChannels);

            // 执行 EventLoop 内部轻量任务
            void ProcessPendingTasks();

            Poller* m_poller;                                     // 轮询器指针
            std::atomic<bool> m_running;                          // 是否运行标志

            std::unordered_set<Channel*> m_channels;             // 管理所有 Channel
            std::mutex m_channelMutex;                            // 保护 m_channels 的互斥锁

            std::vector<std::function<void()>> m_pendingTasks;   // EventLoop 内部任务队列
            std::mutex m_taskMutex;                               // 保护任务队列的互斥锁

            std::shared_ptr<LikesProgram::ThreadPool> m_mainPool; // 指向主线程池
        };


        // 主 EventLoop，负责 accept 和线程池管理
        class MainEventLoop : public EventLoop {
        public:
            explicit MainEventLoop(
                Poller* poller, 
                const std::function<std::shared_ptr<Connection>(SocketType, EventLoop*)>& acceptCallback,
                size_t threadCount = 0, size_t maxPoolSize = 0)
                : EventLoop(poller), m_acceptCallback(acceptCallback) {

                // 创建线程池 若 maxPoolSize 为 0 则会自动改为 (std::thread::hardware_concurrency() ? std::thread::hardware_concurrency() : 1)
                LikesProgram::ThreadPool::Options opts(1, maxPoolSize, 1024);
                m_pool = std::make_shared<LikesProgram::ThreadPool>(opts);
                m_pool->Start();

                // 子 EventLoop 数量
                m_threadCount = threadCount ? threadCount : (std::thread::hardware_concurrency() ? std::thread::hardware_concurrency() : 1);

                // 创建子 EventLoop
                for (int i = 0; i < m_threadCount; ++i) {
                    auto loop = std::make_shared<EventLoop>(poller);
                    loop->SetMainThreadPool(m_pool); // 指向主线程池

                    m_subLoops.push_back(loop); // 添加到容器

                    // 启动 EventLoop
                    std::thread([loop]() { loop->Run(); }).detach();
                }
            }

            std::shared_ptr<LikesProgram::ThreadPool> GetThreadPool() const {
                return m_pool;
            }

        protected:
            void ProcessEvents(const std::vector<Channel*>& activeChannels) override {
                for (Channel* ch : activeChannels) {
                    if (!ch) continue;
                    if (!ch->IsEventEnabled(IOEvent::Read)) continue;

                    // accept 新连接
                    auto conn = m_acceptCallback(ch->GetSocket(), this);
                    if (!conn) continue;

                    // 轮询分配给子 EventLoop
                    auto loop = m_subLoops[m_nextLoop];
                    m_nextLoop = (m_nextLoop + 1) % m_subLoops.size();

                    loop->PostTask([conn, loop]() {
                        auto channel = new Channel(conn->GetSocket(), IOEvent::Read, conn);
                        loop->RegisterChannel(channel);
                    });
                }
            }

        private:
            std::vector<std::shared_ptr<EventLoop>> m_subLoops;
            size_t m_nextLoop = 0;
            size_t m_threadCount;
            std::shared_ptr<LikesProgram::ThreadPool> m_pool;
            std::function<std::shared_ptr<Connection>(SocketType, EventLoop*)> m_acceptCallback;
        };
    }
}
