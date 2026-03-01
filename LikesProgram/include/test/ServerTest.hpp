#pragma once
#include "../LikesProgram/net/Server.hpp"
#include "../LikesProgram/net/pollers/WindowsSelectPoller.hpp"
#include "../LikesProgram/net/pollers/EpollPoller.hpp"
#include "../LikesProgram/Logger.hpp"
#include "../LikesProgram/String.hpp"
#include "../LikesProgram/net/Client.hpp"
#include <iostream>
#include <memory>

using namespace LikesProgram::Net;

// ===== EchoConnection：收到啥回啥 =====
class EchoConnection final : public Connection {
public:
    EchoConnection(SocketType fd, EventLoop* loop, std::unique_ptr<Transport> transport)
        : Connection(fd, loop, std::move(transport)) {
        LOG_DEBUG(u"Conn NEW fd[{}] this[{:P}]", (long long)GetSocket(), (const void*)this);
    }

    ~EchoConnection() {
        LOG_DEBUG(u"Conn DEL fd[{}] this[{:P}]", (long long)GetSocket(), (const void*)this);
    }

protected:
    void OnConnected() override {
        // 可选：欢迎消息
        LOG_WARN(u"Connected fd[{}]", (long long)GetSocket());
        LOG_DEBUG(u"Send Hello Message [echo server: connected]");
        std::string socket = "client[" + std::to_string(GetSocket()) + "]";
        const char* hello = socket.c_str();
        Buffer buffer;
        buffer.Append(hello, std::strlen(hello));
        Send(buffer);
    }

    void OnMessage(Buffer& in) override {
        // 最简单：把当前 buffer 全部回写，然后消费掉
        const auto n = in.ReadableBytes();
        if (n > 0) {
            LOG_INFO(u"OnMessage [{}]", std::string((char*)in.Peek(), in.ReadableBytes()));
            std::string socket = "client[" + std::to_string(GetSocket()) + "]: Message:[";
            Buffer message;
            message.Append(socket.c_str(), socket.size());
            message.Append(in.Peek(), in.ReadableBytes());
            message.Append("]", 1);

            LOG_WARN(u"Broadcast [{}]", std::string((char*)message.Peek(), message.ReadableBytes()));

            std::shared_ptr<Broadcast> broadcast = GetBroadcast();
            Send(in);
            
            // 广播（去除自己）
            broadcast->Send(message.Peek(), message.ReadableBytes(), GetSocket());

            in.Consume(n); // 移除已使用的消息
        }
    }

    void OnClosed() override {
        // 连接关闭
        LOG_WARN(u"Closed fd[{}]", (long long)GetSocket());
    }

    void OnError(int err) override {
        // 错误
        (void)err;
    }
};

// ===== ClientConnection：客户端控制器 =====
class ClientConnection final : public Connection {
public:
    ClientConnection(SocketType fd, EventLoop* loop, std::unique_ptr<Transport> transport)
        : Connection(fd, loop, std::move(transport)) {
        LOG_DEBUG(u"Conn NEW fd[{}] this[{:P}]", (long long)GetSocket(), (const void*)this);
    }

    ~ClientConnection() {
        LOG_DEBUG(u"Conn DEL fd[{}] this[{:P}]", (long long)GetSocket(), (const void*)this);
    }

protected:
    void OnConnected() override {
        // 可选：欢迎消息
        LOG_WARN(u"Connected fd[{}]", (long long)GetSocket());
        LOG_DEBUG(u"Send Hello Message [echo server: connected]");
        std::string socket = "client[" + std::to_string(GetSocket()) + "]";
        const char* hello = socket.c_str();
        Buffer buffer;
        buffer.Append(hello, std::strlen(hello));
        Send(buffer);
    }

    void OnMessage(Buffer& in) override {
        // 最简单：把当前 buffer 全部回写，然后消费掉
        const auto n = in.ReadableBytes();
        if (n > 0) {
            LOG_INFO(u"OnMessage [{}]", std::string((char*)in.Peek(), in.ReadableBytes()));
            std::string socket = "client[" + std::to_string(GetSocket()) + "]: Message:[";
            Buffer message;
            message.Append(socket.c_str(), socket.size());
            message.Append(in.Peek(), in.ReadableBytes());
            message.Append("]", 1);

            LOG_WARN(u"Broadcast [{}]", std::string((char*)message.Peek(), message.ReadableBytes()));

            std::shared_ptr<Broadcast> broadcast = GetBroadcast();
            Send(in);

            // 广播（去除自己）
            broadcast->Send(message.Peek(), message.ReadableBytes(), GetSocket());

            in.Consume(n); // 移除已使用的消息
        }
    }

    void OnClosed() override {
        // 连接关闭
        LOG_WARN(u"Closed fd[{}]", (long long)GetSocket());
    }

    void OnError(int err) override {
        // 错误
        (void)err;
    }
};

namespace ServerTest {
    void Test() {
        // 初始化日志
#ifdef _DEBUG
        auto& logger = LikesProgram::Logger::Instance(true, true);
#else
        auto& logger = LikesProgram::Logger::Instance(true);
#endif
        logger.SetLevel(LikesProgram::Logger::LogLevel::Debug);

#ifdef _WIN32
        logger.SetEncoding(LikesProgram::String::Encoding::GBK);
#endif
        // 控制台输出 Sink
        logger.AddSink(LikesProgram::Logger::CreateConsoleSink()); // 输出到控制台

        // 创建连接器工厂
        ConnectionFactory connectionFactory = [](SocketType fd, EventLoop* ownerLoop) -> std::shared_ptr<Connection> {
            auto transport = std::make_unique<TcpTransport>(fd);
            return std::make_shared<EchoConnection>(fd, ownerLoop, std::move(transport));
        };


        std::vector<String> addrs = { u"0.0.0.0", u"::" };
        unsigned short port = 8080;
        size_t subLoops = 16;
        LOG_DEBUG(u"EchoServer listening on port [{}] subLoops [{}]", (size_t)port, subLoops);

        Server server(addrs, port, connectionFactory, subLoops);

        /* 自定义 轮询器

        // 创建轮询器工厂
        PollerFactory pollerFactory = []() -> std::unique_ptr<Poller> {
            // 这里可以自行修改，只需要返回 std::unique_ptr<Poller> 即可，其中 自定义的沦陷器 需要继承 Poller
#ifdef _WIN32
            return std::make_unique<WindowsSelectPoller>(nullptr);
#else
            return std::make_unique<EpollPoller>(nullptr);
#endif
        };

        Server server(addrs, port, pollerFactory, connectionFactory, subLoops);

        */

        server.Start(); // 启动服务

        // 等待 服务器 启动完成
        //Sleep(1000);

        // 创建客户端
        Client client(addrs[1], port, connectionFactory, subLoops);
        client.Start(); // 开始，连接
        while (1) {
            std::cout << "TestClient:" << std::endl;
            std::string com;
            std::cin >> com;

            if (com == "exit") {
                client.Shutdown(); // 关闭连接
                break;
            }

            client.Send(com);
        }
        //// 等待 客户端 关闭
        //Sleep(1000);

        std::thread t([port, subLoops, &server] {
            // 模拟其他业务处理
            std::this_thread::sleep_for(std::chrono::seconds(20)); // 两分钟后自动停止
            server.Shutdown(); // 停止服务
        });

        // 阻塞线程
        server.WaitShutdown(); // 等待 Server 停止

        LOG_DEBUG(u"EchoServer Shutdown");

        // 其他业务 ...

        t.join(); // 等待线程结束
        logger.Shutdown();
    }
}
