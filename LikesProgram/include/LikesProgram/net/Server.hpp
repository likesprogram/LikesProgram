#pragma once
#include "../system/LikesProgramLibExport.hpp"
#include "EventLoop.hpp"
#include "MainEventLoop.hpp"
#include "Connection.hpp"
#include "Channel.hpp"
#include "Transport.hpp"

namespace LikesProgram {
    namespace Net {
        using PollerFactory = EventLoop::PollerFactory;
        using ConnectionFactory = MainEventLoop::ConnectionFactory;

        // Server 基类
        class Server {
        public:
            // 构造函数，创建监听 Channel 并注册到 MainEventLoop
            explicit Server(unsigned short port, PollerFactory pollerFactory, ConnectionFactory connectionFactory, size_t subLoopCount = 0);
            ~Server();

            // 启动：先 start sub loops，再跑 main loop（阻塞）
            void Run();
            void Stop();

            // 广播
            void Broadcast(const void* data, size_t len, const std::vector<SocketType>& removeSockets) const;
        private:

            // 创建监听 socket
            SocketType Listen();

            // 设置 socket 为非阻塞模式
            int SetNonBlocking(SocketType fdOrSocket);

        protected:
            std::shared_ptr<MainEventLoop> m_mainLoop; // 主事件循环对象
            SocketType m_listenFd = kInvalidSocket; // 监听 socket fd
            std::unique_ptr<Channel> m_listenChannel;
            unsigned short m_port = 0;                // 监听端口号
        };
    }
}
