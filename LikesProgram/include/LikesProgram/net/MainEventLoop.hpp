#pragma once
#pragma once
#include "EventLoop.hpp"
#include "Channel.hpp"
#include "IOEvent.hpp"
#include <functional>
#include <memory>
#include <thread>
#include <vector>
#include <atomic>

namespace LikesProgram {
    namespace Net {
        class Connection;
        class MainEventLoop final : public EventLoop {
        public:
            using ConnectionFactory = std::function<std::shared_ptr<Connection>(SocketType, EventLoop*)>;
            MainEventLoop(Server* server, PollerFactory subPollerFactory,
                ConnectionFactory subConnectionFactory,
                size_t subLoopCount = 0);

            ~MainEventLoop() override;

            // 启动所有 sub loops（主 loop 仍由外部调用 Run()）
            void StartSubLoops();

            // 停止 sub loops（会调用 Stop 并 Join）
            void StopSubLoops();

            // 广播
            void Broadcast(const void* data, size_t len, const std::vector<SocketType>& removeSockets) override;
        protected:
            // main loop 的事件处理：只处理 accept
            void ProcessEvents(const std::vector<Channel*>& activeChannels) override;

        private:
            std::shared_ptr<EventLoop> PickSubLoopRoundRobin();

        private:
            PollerFactory m_subPollerFactory;
            ConnectionFactory m_subConnectionFactory;

            std::vector<std::shared_ptr<EventLoop>> m_subLoops;
            std::vector<std::thread> m_subThreads;

            std::atomic<size_t> m_rr = 0;
            size_t m_subLoopCount = 0;
            bool m_subStarted = false;
        };
    }
}
