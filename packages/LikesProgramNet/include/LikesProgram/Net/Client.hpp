#pragma once
#include <LikesProgram/Net/system/LikesProgramNetExport.hpp>
#include <LikesProgram/Net/Address.hpp>
#include <LikesProgram/Net/ConnectionFactory.hpp>
#include <LikesProgram/Net/Connection.hpp>
#include <cstdint>
#include <memory>

namespace LikesProgram {
    namespace Net {
        class LIKESPROGRAM_NET_API Client {
        public:
            enum class Status : std::uint8_t {
                Stopped,
                Connecting,
                Connected,
                Stopping
            };

            // 创建连接到指定远端地址的 TCP 客户端。
            Client(const Address& remoteAddress, ConnectionFactory factory);
            // 创建连接到指定远端地址的客户端，并选择 TCP/UDP 传输族。
            Client(const Address& remoteAddress, TransportKind transportKind, ConnectionFactory factory);
            // 析构时停止后台事件循环。
            ~Client();

            Client(const Client&) = delete;
            Client& operator=(const Client&) = delete;

            // 启动连接并创建后台 EventLoop 线程。
            void Start();
            // 阻塞等待 Shutdown 完成。
            void WaitShutdown() const noexcept;
            // 停止客户端并关闭连接。
            void Shutdown();
            // 返回当前状态。
            Status GetStatus() const noexcept;
            // 返回当前连接，连接失败或停止后可能为空。
            std::shared_ptr<Connection> GetConnection() const;

        private:
            struct ClientImpl;

            // 原子设置状态并通知等待者。
            void SetStatus(Status status);

            ClientImpl* m_impl = nullptr;                  // 客户端实现，隐藏线程/锁/连接状态
        };
    }
}
