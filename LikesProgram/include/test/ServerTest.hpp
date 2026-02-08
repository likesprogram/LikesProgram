#pragma once
#include "../LikesProgram/net/Server.hpp"
#include "../LikesProgram/net/pollers/WindowsSelectPoller.hpp"
#include "../LikesProgram/net/pollers/EpollPoller.hpp"
#include "../LikesProgram/Logger.hpp"
#include "../LikesProgram/String.hpp"
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
        LOG_DEBUG(u"Connected fd[{}]", (long long)GetSocket());
        LOG_DEBUG(u"Send Hello Message [echo server: connected]");
        const char* hello = "echo server: connected\r\n";
        Buffer buffer;
        buffer.Append(hello, std::strlen(hello));
        Send(buffer);
    }

    void OnMessage(Buffer& in) override {
        // 最简单：把当前 buffer 全部回写，然后消费掉
        const auto n = in.ReadableBytes();
        if (n > 0) {
            LOG_INFO(u"OnMessage [{}]", (char*)in.Peek());
            const char* message = "Server Message";
            in.Append(message, std::strlen(message));
            Send(in);
            in.Consume(n);
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

        // 创建轮询器工厂
        PollerFactory pollerFactory = []() -> std::unique_ptr<Poller> {
#ifdef _WIN32
            return std::make_unique<WindowsSelectPoller>(nullptr);
#else
            return std::make_unique<EpollPoller>(nullptr);
#endif
        };

        // 创建连接器工厂
        ConnectionFactory connectionFactory = [](SocketType fd, EventLoop* ownerLoop) -> std::shared_ptr<Connection> {
            auto transport = std::make_unique<TcpTransport>(fd);
            return std::make_shared<EchoConnection>(fd, ownerLoop, std::move(transport));
        };

        unsigned short port = 8080;
        size_t subLoops = 16;

        Server server(
            port,
            pollerFactory,
            connectionFactory,
            subLoops
        );
        
        LOG_DEBUG(u"EchoServer listening on port [{}] subLoops [{}]", (size_t)port, subLoops);

        server.Run(); // 阻塞
        logger.Shutdown();
    }
}
