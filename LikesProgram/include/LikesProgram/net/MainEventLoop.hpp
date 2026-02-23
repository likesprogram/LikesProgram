#pragma once
#include "EventLoop.hpp"
#include "Channel.hpp"
#include "IOEvent.hpp"
#include "Broadcast.hpp"
#include <functional>
#include <memory>
#include <thread>
#include <vector>
#include <atomic>
#include <span>

namespace LikesProgram {
    namespace Net {
        class Connection;
        class MainEventLoop final : public EventLoop {
        public:
            using ConnectionFactory = std::function<std::shared_ptr<Connection>(SocketType, EventLoop*)>;
            MainEventLoop(PollerFactory subPollerFactory, ConnectionFactory subConnectionFactory, size_t subLoopCount = 0);

            ~MainEventLoop() override;

            // 启动所有 sub loops（主 loop 仍由外部调用 Run()）
            void StartSubLoops();

            // 停止 sub loops（会调用 Stop 并 Join）
            void StopSubLoops();

            // 获取 所有 sub loops
            std::span<const std::shared_ptr<EventLoop>> GetSubLoops() const noexcept;

            // 设置广播器
            void SetBroadcast(std::shared_ptr<Broadcast> broadcast) noexcept;

            // 获取广播器
            std::shared_ptr<Broadcast> GetBroadcast() noexcept;
        protected:
            // main loop 的事件处理：只处理 accept
            void ProcessEvents(const std::vector<Channel*>& activeChannels) override;

        private:
            std::shared_ptr<EventLoop> PickSubLoopRoundRobin();

        private:
            PollerFactory m_subPollerFactory;
            ConnectionFactory m_subConnectionFactory;

            std::shared_ptr<Broadcast> m_broadcast = nullptr;    // 广播器
            std::vector<std::shared_ptr<EventLoop>> m_subLoops;
            std::vector<std::thread> m_subThreads;

            std::atomic<size_t> m_rr = 0;
            size_t m_subLoopCount = 0;
            bool m_subStarted = false;
        };
    }
}
