#pragma once
#include <LikesProgram/Net/system/LikesProgramNetExport.hpp>
#include <LikesProgram/Net/Address.hpp>
#include <LikesProgram/Net/Connection.hpp>
#include <LikesProgram/Net/ConnectionFactory.hpp>
#include <LikesProgram/Net/Protocol.hpp>
#include <chrono>
#include <cstddef>
#include <memory>

namespace LikesProgram {
    namespace Net {
        namespace Internal {
            class ConnectionPoolState;
        }

        struct ConnectionPoolOptions {
            Address remoteAddress;                                      // TCP 远端地址
            TransportKind transportKind = TransportKind::Tcp;           // 仅允许 Tcp，保留字段用于显式拒绝 UDP 池化
            ConnectionFactory factory;                                  // 用户连接工厂，空工厂回退到 completion-owned 明文 TCP
            std::size_t maxConnections = 4;                             // 最大并发 TCP 连接数
            std::size_t maxIdleConnections = 4;                         // 最大空闲复用连接数
            std::size_t workerThreads = 1;                              // 复用 EventLoopGroup 承载 outbound 连接 I/O
            std::chrono::milliseconds acquireTimeout{ 1000 };           // Acquire 默认等待时间
        };

        struct ConnectionPoolStats {
            std::size_t activeConnections = 0;                          // 已借出的连接数
            std::size_t idleConnections = 0;                            // 可复用空闲连接数
            std::size_t totalConnections = 0;                           // 池内已知连接数，含建连中的保留名额
            std::size_t waitingAcquires = 0;                            // 正在等待名额的 Acquire 调用数
        };

        class ConnectionPool;

        class LIKESPROGRAM_NET_API ConnectionLease {
        public:
            // 创建空租约。
            ConnectionLease();
            // 移动租约会转移自动归还责任。
            ConnectionLease(ConnectionLease&& other) noexcept;
            // 析构时将连接归还池；Discard 后会关闭连接。
            ~ConnectionLease();

            ConnectionLease(const ConnectionLease&) = delete;
            ConnectionLease& operator=(const ConnectionLease&) = delete;

            // 移动赋值前先归还当前持有的连接。
            ConnectionLease& operator=(ConnectionLease&& other) noexcept;

            // 返回租约持有的连接快照。
            std::shared_ptr<Connection> Get() const;
            // 便捷访问底层连接。
            Connection* operator->() const noexcept;
            // 返回租约是否持有可访问连接。
            explicit operator bool() const noexcept;
            // 提前归还连接，连接仍可复用。
            void Release();
            // 丢弃连接并归还名额，通常用于协议错误或上层判定不可复用。
            void Discard();

        private:
            friend class Internal::ConnectionPoolState;
            struct ConnectionLeaseImpl;

            // 由 ConnectionPool 创建真实租约。
            ConnectionLease(ConnectionLeaseImpl* impl);
            // 统一释放当前租约。
            void ReleaseCurrent(bool reusable) noexcept;

            ConnectionLeaseImpl* m_impl = nullptr;                      // 租约实现，隐藏 shared_ptr 和池状态
        };

        class LIKESPROGRAM_NET_API ConnectionPool {
        public:
            // 创建 TCP outbound 连接池；transportKind 为 Udp 时抛出异常。
            explicit ConnectionPool(const ConnectionPoolOptions& options);
            // 析构时关闭空闲连接并停止 worker EventLoopGroup。
            ~ConnectionPool();

            ConnectionPool(const ConnectionPool&) = delete;
            ConnectionPool& operator=(const ConnectionPool&) = delete;

            // 按 options.acquireTimeout 获取连接；超时会抛出 runtime_error。
            ConnectionLease Acquire();
            // 在给定时间内尝试获取连接，超时返回空租约。
            ConnectionLease TryAcquire(std::chrono::milliseconds timeout);
            // 主动关闭池并唤醒等待者。
            void Shutdown();
            // 返回当前池状态快照。
            ConnectionPoolStats Stats() const;

        private:
            struct ConnectionPoolImpl;

            ConnectionPoolImpl* m_impl = nullptr;                       // 连接池实现，隐藏队列/锁/worker 组
        };
    }
}
