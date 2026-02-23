#pragma once
#include "EventLoop.hpp"
#include "Channel.hpp"
#include "Connection.hpp"
#include "Transport.hpp"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <cstdint>
#include <chrono>

namespace LikesProgram {
    namespace Net {
        using ConnectionFactory = EventLoop::ConnectionFactory;

        class Client {
        public:
            using ConnectedCallback = std::function<void(Connection&)>;
            using ConnectFailedCallback = std::function<void(int err, const std::string& what)>;
            using DisconnectedCallback = std::function<void()>;

        public:
            Client(std::string host, unsigned short port, ConnectionFactory factory, size_t subLoopCount = 0);

            ~Client();

            Client(const Client&) = delete;
            Client& operator=(const Client&) = delete;

            // 异步：会切到 loop 线程
            void Run();
            void Stop();

            // 业务侧常用：获取当前连接（可能为空）
            Connection* GetConnection() const noexcept;

            // 发送：若未连接返回 false（最小可用）
            bool Send(std::string_view data);
            bool Send(const Buffer& buf);

            // 设置回调
            void SetOnConnected(ConnectedCallback cb);
            void SetOnConnectFailed(ConnectFailedCallback cb);
            void SetOnDisconnected(DisconnectedCallback cb);

        private:
            enum class State : uint8_t { Idle, Connecting, Connected, Stopping, Closed };

        private:
            void RunInLoop();
            void StopInLoop();

            // 连接阶段
            void BeginConnect();                 // create fd + nonblock connect
            void OnConnectEvent();               // writable -> check SO_ERROR
            void FinishConnectSuccess();
            void FinishConnectFail(int err, const char* why);

            // 断开/清理
            void ResetConnectingChannel();
            void ResetConnection();

            // 重连策略（最小实现：loop 里定时重试；如果你还没 TimerQueue，可以先外部驱动或用 loop 的 task + sleep 方案替代）
            void ScheduleReconnect();

        private:
            EventLoop* m_loop{};
            std::string m_host;
            uint16_t m_port{};

            State m_state{ State::Idle };

            SocketType m_connectFd = kInvalidSocket;
            std::unique_ptr<Channel> m_connectChannel;

            std::unique_ptr<Connection> m_conn;

            ConnectionFactory m_factory;

            ConnectedCallback m_onConnected;
            ConnectFailedCallback m_onConnectFailed;
            DisconnectedCallback m_onDisconnected;

            std::chrono::milliseconds m_reconnectDelay{ 0 };
        };
    }
}