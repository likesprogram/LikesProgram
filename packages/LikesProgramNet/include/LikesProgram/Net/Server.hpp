#pragma once
#include <LikesProgram/Net/system/LikesProgramNetExport.hpp>
#include <LikesProgram/Net/Address.hpp>
#include <LikesProgram/Net/Channel.hpp>
#include <LikesProgram/Net/ConnectionFactory.hpp>
#include <LikesProgram/Net/Protocol.hpp>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace LikesProgram {
    namespace Net {
        class LIKESPROGRAM_NET_API Server {
        public:
            enum class Status : std::uint8_t {
                Stopped,
                Starting,
                Running,
                Stopping
            };

            // 监听单个地址。
            Server(const Address& listenAddress, ConnectionFactory factory);
            // 监听单个地址，并选择 TCP/UDP 传输族。
            Server(const Address& listenAddress, TransportKind transportKind, ConnectionFactory factory);
            // 监听多个地址。
            Server(const std::vector<Address>& listenAddresses, ConnectionFactory factory);
            // 监听多个地址，并选择 TCP/UDP 传输族。
            Server(const std::vector<Address>& listenAddresses, TransportKind transportKind, ConnectionFactory factory);
            // 析构时关闭监听 socket 和事件循环。
            ~Server();

            Server(const Server&) = delete;
            Server& operator=(const Server&) = delete;

            // 启动监听和后台 EventLoop。
            void Start();
            // 阻塞等待 Shutdown 完成。
            void WaitShutdown() const noexcept;
            // 停止服务器。
            void Shutdown();
            // 返回当前状态。
            Status GetStatus() const noexcept;
            // 返回实际监听地址，端口为 0 时可在 Start 后读取系统分配端口。
            std::vector<Address> GetListenAddresses() const;
            // 设置 worker EventLoop 数量；必须在 Start 前调用，0 表示单 Reactor。
            void SetWorkerThreads(std::size_t workerCount);

        private:
            struct ServerImpl;

            // 创建并绑定所有监听 socket。
            void Listen();
            // 消费一个 accept completion 并创建连接。
            void HandleAccepted(SocketType clientFd, EventLoop* ownerLoop);
            // 为 UDP 监听 socket 创建连接对象。
            void AttachDatagramConnection(SocketType fd);
            // 选择连接所属 EventLoop，未启用 worker 时回退到主 loop。
            EventLoop* PickConnectionLoop() noexcept;
            // 在指定 loop 持有并启动连接。
            void AttachConnectionToLoop(const std::shared_ptr<Connection>& connection, EventLoop* ownerLoop);
            // 原子设置状态并通知等待者。
            void SetStatus(Status status);

            ServerImpl* m_impl = nullptr;                      // 服务端实现，隐藏监听/线程/连接状态
        };
    }
}
