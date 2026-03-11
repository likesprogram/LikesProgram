#pragma once
#include "EventLoop.hpp"
#include "Channel.hpp"
#include "Connection.hpp"
#include "Transport.hpp"
#include "Address.hpp"
#include "../String.hpp"
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <cstdint>
#include <chrono>

namespace LikesProgram {
    namespace Net {
        using PollerFactory = EventLoop::PollerFactory;
        using ConnectionFactory = EventLoop::ConnectionFactory;

        class Client {
        public:
            enum class Status : uint8_t {
                Stopped,    // 停止
                Connecting, // 正在连接
                Connected,  // 连接成功正在通信
                Stopping    // 正在停止
            };
            explicit Client(const Address& remoteAddr, ConnectionFactory factory);

            ~Client();

            Client(const Client&) = delete;
            Client& operator=(const Client&) = delete;

            // 启动连接
            void Start();

            // 在 Start 之后调用，阻塞，直到 Shutdown 被调用
            void WaitShutdown() const noexcept;

            // 关闭
            void Shutdown();

            // 状态查询
            Status GetStatus() const noexcept;

            // 业务侧常用：获取当前连接（可能为空）
            std::shared_ptr<Connection> GetConnection() const noexcept;
        private:
            // 连接阶段
            void BeginConnect();                 // create fd + nonblock connect
            void OnConnectEvent();               // writable -> check SO_ERROR
            void FinishConnectSuccess();
            void FinishConnectFail(int err, const char* why);

            // 重连策略
            void ScheduleReconnect();

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
            std::thread m_loopThread;
            std::shared_ptr<EventLoop> m_loop;
            std::vector<Address> m_remoteAddrs; // 远程地址

            std::atomic<Status> m_status = Status::Stopped;
            mutable std::mutex m_stateMutex;           // 保护 Start/Shutdown/WaitShutdown 的状态切换

            SocketType m_connectFd = kInvalidSocket;
            std::shared_ptr<Channel> m_connectChannel;

            // 连接器
            std::shared_ptr<Connection> m_conn;

            ConnectionFactory m_factory;

            std::thread m_connectWaiter;
            std::atomic_bool m_stopWaiter{ false };

            mutable std::condition_variable m_stateCv;

            std::chrono::milliseconds m_reconnectDelay{ 0 };
        };
    }
}