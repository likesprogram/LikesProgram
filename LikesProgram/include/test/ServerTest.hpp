#pragma once
#include "../LikesProgram/net/Server.hpp"
#include "../LikesProgram/net/pollers/WindowsSelectPoller.hpp"
#include "../LikesProgram/net/pollers/EpollPoller.hpp"
#include "../LikesProgram/log/Logger.hpp"
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
        LogDebug(u"Server new conn[{}] this[{:P}]", (long long)GetSocket(), (const void*)this);
    }

    ~EchoConnection() {
        LogDebug(u"Server del conn[{}] this[{:P}]", (long long)GetSocket(), (const void*)this);
    }

protected:
    void OnConnected() override {
        // 可选：欢迎消息
        LogDebug(u"Send Hello Message [echo server: connected]");
        std::string socket = "Server socket: " + std::to_string(GetSocket());
        const char* hello = socket.c_str();
        Buffer buffer;
        buffer.Append(hello, std::strlen(hello));
        Send(buffer);
    }

    void OnMessage(Buffer& in) override {
        // 最简单：把当前 buffer 全部回写，然后消费掉
        const auto n = in.ReadableBytes();
        if (n > 0) {
            LogInfo(u"Server OnMessage [{}]", std::string((char*)in.Peek(), in.ReadableBytes()));
            std::string socket = "Client[" + std::to_string(GetSocket()) + "]: Message:[";
            Buffer message;
            message.Append(socket.c_str(), socket.size());
            message.Append(in.Peek(), in.ReadableBytes());
            message.Append("]", 1);

            LogWarn(u"Server broadcast [{}]", std::string((char*)message.Peek(), message.ReadableBytes()));

            std::shared_ptr<Broadcast> broadcast = GetBroadcast();
            Send(in);
            
            // 广播（去除自己）
            broadcast->Send(message.Peek(), message.ReadableBytes(), GetSocket());

            in.Consume(n); // 移除已使用的消息
        }
    }

    void OnClosed() override {
        // 连接关闭
        LogWarn(u"Server closed fd[{}]", (long long)GetSocket());
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
        LogDebug(u"Client new conn[{}] this[{:P}]", (long long)GetSocket(), (const void*)this);
    }

    ~ClientConnection() {
        LogDebug(u"Client del conn[{}] this[{:P}]", (long long)GetSocket(), (const void*)this);
    }

protected:
    void OnConnected() override {
        // 可选：欢迎消息
        LogDebug(u"Send Hello Message [echo client: connected]");
        std::string socket = "Client socket: " + std::to_string(GetSocket());
        const char* hello = socket.c_str();
        Buffer buffer;
        buffer.Append(hello, std::strlen(hello));
        Send(buffer);
    }

    void OnMessage(Buffer& in) override {
        // 最简单：把当前 buffer 全部回写，然后消费掉
        const auto n = in.ReadableBytes();
        if (n > 0) {
            LogInfo(u"ClientOnMessage [{}]", std::string((char*)in.Peek(), in.ReadableBytes()));

            in.Consume(n); // 移除已使用的消息
        }
    }

    void OnClosed() override {
        // 连接关闭
        LogWarn(u"Client closed fd[{}]", (long long)GetSocket());
    }

    void OnError(int err) override {
        // 错误
        (void)err;
    }
};

namespace ServerTest {
    void Test() {
#ifdef _DEBUG
        auto& logger = LikesProgram::Log::Logger::Instance(true, true);
#else
        auto& logger = LikesProgram::Log::Logger::Instance(true);
#endif

#ifdef _WIN32
        logger.SetEncoding(LikesProgram::String::Encoding::GBK);
#endif
        logger.SetLevel(LikesProgram::Log::Level::Trace);
        // 内置控制台输出 Sink
        logger.AddSink(LikesProgram::Log::ConsoleSink::CreateSink()); // 输出到控制台

        // 创建连接器工厂
        ConnectionFactory connectionFactory = [](SocketType fd, EventLoop* ownerLoop) -> std::shared_ptr<Connection> {
            auto transport = std::make_unique<TcpTransport>(fd);
            return std::make_shared<EchoConnection>(fd, ownerLoop, std::move(transport));
        };


        std::vector<String> addrs = { u"0.0.0.0", u"::" };
        unsigned short port = 8080;
        size_t subLoops = 16;
        LogDebug(u"EchoServer listening on port [{}] subLoops [{}]", (size_t)port, subLoops);

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
        std::this_thread::sleep_for(std::chrono::seconds(2));

        // 创建连接器工厂
        ConnectionFactory clientConnectionFactory = [](SocketType fd, EventLoop* ownerLoop) -> std::shared_ptr<Connection> {
            auto transport = std::make_unique<TcpTransport>(fd);
            return std::make_shared<ClientConnection>(fd, ownerLoop, std::move(transport));
        };
        // 创建客户端
        Client client(u"127.0.0.1", port, clientConnectionFactory);
        client.Start(); // 开始，连接
        // 等待 客户端 启动完成
        std::this_thread::sleep_for(std::chrono::seconds(2));
        while (client.GetStatus() == Client::Status::Connected) {
            std::cout << "TestClient:" << std::endl;
            std::string com;
            std::cin >> com;

            if (com == "exit") {
                std::string bye = "Bye!";

                client.GetConnection()->Send(bye.c_str(), bye.size());
                // 等待消息完成
                std::this_thread::sleep_for(std::chrono::seconds(2));

                client.Shutdown(); // 关闭连接
                break;
            }
            else if (com == "rect") {
                client.Shutdown(); // 关闭连接
                std::this_thread::sleep_for(std::chrono::seconds(2));
                client.Start(); // 重新连接
                std::this_thread::sleep_for(std::chrono::seconds(2));
                continue;
            }

            client.GetConnection()->Send(com.c_str(), com.size());
        }

        server.Shutdown(); // 停止服务

        // 阻塞线程
        //server.WaitShutdown(); // 等待 Server 停止

        LogDebug(u"EchoServer Shutdown");

        logger.Shutdown();
    }
}
