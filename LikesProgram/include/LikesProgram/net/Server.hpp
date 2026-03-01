#pragma once
#include "../system/LikesProgramLibExport.hpp"
#include "EventLoop.hpp"
#include "MainEventLoop.hpp"
#include "Connection.hpp"
#include "Channel.hpp"
#include "Transport.hpp"
#include "Broadcast.hpp"
#include "../String.hpp"
#include <condition_variable>

namespace LikesProgram {
    namespace Net {
        using PollerFactory = EventLoop::PollerFactory;
        using ConnectionFactory = EventLoop::ConnectionFactory;

        // Server 基类
        class Server {
        public:
            enum class Status : uint8_t {
                Stopped,    // 停止
                Starting,   // 正在启动
                Running,    // 运行中
                Stopping    // 正在停止
            };
            // 构造函数，创建监听 Channel 并注册到 MainEventLoop
            explicit Server(std::vector<String>& addrs, unsigned short port, ConnectionFactory connectionFactory, size_t subLoopCount = 0);
            // 自定义轮询器
            explicit Server(std::vector<String>& addrs, unsigned short port, PollerFactory pollerFactory, ConnectionFactory connectionFactory, size_t subLoopCount = 0);
            ~Server();

            // 启动
            void Start();

            // 在 Start 之后调用，阻塞，直到 Shutdown 被调用
            void WaitShutdown() const noexcept;

            // 停止
            void Shutdown();

            // 状态查询
            Status GetStatus() const noexcept;

            // 获取广播器
            std::shared_ptr<Broadcast> GetBroadcast() noexcept;
        private:

            // 创建监听 socket
            bool Listen(std::vector<String>& addrs);

            // 设置 socket 为非阻塞模式
            int SetNonBlocking(SocketType fdOrSocket);

            // 设置状态，store + notify_all
            void SetStatus(Status status);

            bool StatusEquals(Status status) const noexcept;

            template<typename... Ts>
            bool StatusAnyOf(Status first, Ts... rest) const noexcept {
                static_assert((std::is_same_v<Ts, Status> && ...), "All arguments must be Status");
                const Status cur = m_status.load(std::memory_order_acquire);
                return ((cur == first) || ... || (cur == rest));
            }

            template<typename... Ts>
            bool StatusAllOf(Status first, Ts... rest) const noexcept {
                static_assert((std::is_same_v<Ts, Status> && ...), "All arguments must be Status");
                const Status cur = m_status.load(std::memory_order_acquire);
                return ((cur == first) && ... && (cur == rest));
            }
        private:
            std::shared_ptr<MainEventLoop> m_mainLoop; // 主事件循环对象
            std::vector<SocketType> m_listenFds;
            std::vector<std::unique_ptr<Channel>> m_listenChannels;
            unsigned short m_port = 0;                 // 监听端口号

            std::thread m_mainThread;                  // 线程
            std::atomic<Status> m_status = Status::Stopped;
            mutable std::mutex m_stateMutex;           // 保护 Start/Shutdown/WaitShutdown 的状态切换

            mutable std::condition_variable m_stateCv;
        };
    }
}
