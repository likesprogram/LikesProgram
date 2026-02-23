#pragma once
#include "../system/LikesProgramLibExport.hpp"
#include "EventLoop.hpp"
#include "MainEventLoop.hpp"
#include "Connection.hpp"
#include "Channel.hpp"
#include "Transport.hpp"
#include "Broadcast.hpp"
#include "../String.hpp"

namespace LikesProgram {
    namespace Net {
        using PollerFactory = EventLoop::PollerFactory;
        using ConnectionFactory = EventLoop::ConnectionFactory;

        // Server 基类
        class Server {
        public:
            // 构造函数，创建监听 Channel 并注册到 MainEventLoop
            explicit Server(std::vector<String>& addrs, unsigned short port, ConnectionFactory connectionFactory, size_t subLoopCount = 0);
            // 自定义轮询器
            explicit Server(std::vector<String>& addrs, unsigned short port, PollerFactory pollerFactory, ConnectionFactory connectionFactory, size_t subLoopCount = 0);
            ~Server();

            // 启动：先 start sub loops，再跑 main loop（阻塞）
            void Run();
            void Stop();

            // 获取广播器
            std::shared_ptr<Broadcast> GetBroadcast() noexcept;
        private:

            // 创建监听 socket
            bool Listen(std::vector<String>& addrs);

            // 设置 socket 为非阻塞模式
            int SetNonBlocking(SocketType fdOrSocket);

        private:
            std::shared_ptr<MainEventLoop> m_mainLoop; // 主事件循环对象
            std::vector<SocketType> m_listenFds;
            std::vector<std::unique_ptr<Channel>> m_listenChannels;
            unsigned short m_port = 0;                // 监听端口号
        };
    }
}
