#include <LikesProgram/Net/Server.hpp>
#include <LikesProgram/Net/Connection.hpp>
#include <LikesProgram/Net/EventLoop.hpp>
#include <LikesProgram/Net/TcpTransport.hpp>
#include <LikesProgram/Net/UdpTransport.hpp>
#include "net/SocketOps.hpp"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace LikesProgram {
    namespace Net {
        namespace {
            int SocketTypeForTransport(TransportKind kind) {
                // TCP 使用监听流 socket，UDP 使用绑定后的数据报 socket。
                return IsDatagramTransport(kind) ? SOCK_DGRAM : SOCK_STREAM;
            }

            int SocketProtocolForTransport(TransportKind kind) {
                // 明确协议号，兼容 Winsock 对 type/protocol 组合的校验。
                return IsDatagramTransport(kind) ? IPPROTO_UDP : IPPROTO_TCP;
            }

            std::shared_ptr<Connection> CreateDefaultServerConnection(
                SocketType fd,
                EventLoop* loop,
                TransportKind kind) {
                // Net 内置 TCP/UDP 明文传输；安全层可由用户派生 transport 后通过 factory 注入。
                if (kind == TransportKind::Tcp) {
                    return std::make_shared<Connection>(fd, loop, std::make_unique<TcpTransport>(fd));
                }

                if (kind == TransportKind::Udp) {
                    return std::make_shared<Connection>(fd, loop, std::make_unique<UdpTransport>(fd));
                }

                return {};
            }
        }

        struct Server::ServerImpl {
            std::vector<Address> m_requestedAddresses;         // 用户请求监听地址
            std::vector<Address> m_boundAddresses;             // 实际绑定地址
            TransportKind m_transportKind = TransportKind::Tcp; // 服务端使用的传输族
            ConnectionFactory m_factory;                       // 新连接工厂
            std::shared_ptr<EventLoop> m_loop;                 // 服务器事件循环
            std::vector<SocketType> m_listenFds;               // 监听 socket 集合
            std::vector<std::unique_ptr<Channel>> m_channels;  // 监听 socket 的 Channel
            std::vector<std::shared_ptr<Connection>> m_datagramConnections; // UDP 监听连接
            std::thread m_loopThread;                          // EventLoop 线程
            std::atomic<Status> m_status{ Status::Stopped };   // 服务器生命周期状态
            mutable std::mutex m_stateMutex;                   // 保护状态与地址快照
            mutable std::condition_variable m_stateCv;         // Shutdown 等待条件
        };

        Server::Server(const Address& listenAddress, ConnectionFactory factory)
            : Server(std::vector<Address>{ listenAddress }, TransportKind::Tcp, std::move(factory)) {
        }

        Server::Server(const Address& listenAddress, TransportKind transportKind, ConnectionFactory factory)
            : Server(std::vector<Address>{ listenAddress }, transportKind, std::move(factory)) {
        }

        Server::Server(const std::vector<Address>& listenAddresses, ConnectionFactory factory)
            : Server(listenAddresses, TransportKind::Tcp, std::move(factory)) {
        }

        Server::Server(
            const std::vector<Address>& listenAddresses,
            TransportKind transportKind,
            ConnectionFactory factory)
            : m_impl(new ServerImpl{}) {
            m_impl->m_requestedAddresses = listenAddresses;
            m_impl->m_transportKind = transportKind;
            m_impl->m_factory = std::move(factory);
        }

        Server::~Server() {
            Shutdown();
            delete m_impl;
            m_impl = nullptr;
        }

        void Server::Start() {
            if (m_impl == nullptr) return;

            std::lock_guard<std::mutex> lock(m_impl->m_stateMutex); // 串行化启动状态切换
            if (m_impl->m_status.load(std::memory_order_acquire) != Status::Stopped) return;

            SetStatus(Status::Starting);
            if (!m_impl->m_factory.InitializeSharedSecureResources()) {
                SetStatus(Status::Stopped);
                throw std::runtime_error("Server shared secure resources initialization failed");
            }

            m_impl->m_loop = std::make_shared<EventLoop>();
            Listen();

            for (SocketType fd : m_impl->m_listenFds) {
                if (IsDatagramTransport(m_impl->m_transportKind)) {
                    // UDP 没有 accept 阶段，一个绑定 socket 对应一个长期连接对象。
                    AttachDatagramConnection(fd);
                    continue;
                }

                auto channel = std::make_unique<Channel>(m_impl->m_loop.get(), fd, IOEvent::None);
                channel->SetReadCallback([this, fd]() { AcceptReady(fd); });
                (void)m_impl->m_loop->RegisterChannel(channel.get());
                channel->EnableReading();
                m_impl->m_channels.push_back(std::move(channel));
            }

            auto loop = m_impl->m_loop; // 后台线程持有事件循环生命周期
            m_impl->m_loopThread = std::thread([loop]() {
                loop->Start();
            });

            SetStatus(Status::Running);
        }

        void Server::WaitShutdown() const noexcept {
            if (m_impl == nullptr) return;

            std::unique_lock<std::mutex> lock(m_impl->m_stateMutex); // 等待状态回到 Stopped
            m_impl->m_stateCv.wait(lock, [this]() {
                return m_impl->m_status.load(std::memory_order_acquire) == Status::Stopped;
            });
        }

        void Server::Shutdown() {
            if (m_impl == nullptr) return;

            std::shared_ptr<EventLoop> loop; // 当前事件循环快照
            std::vector<std::shared_ptr<Connection>> datagramConnections; // UDP 连接快照
            {
                std::lock_guard<std::mutex> lock(m_impl->m_stateMutex);
                const Status status = m_impl->m_status.load(std::memory_order_acquire);
                if (status == Status::Stopped) return;
                SetStatus(Status::Stopping);
                loop = m_impl->m_loop;
                datagramConnections = m_impl->m_datagramConnections;
            }

            for (const auto& connection : datagramConnections) {
                if (connection) connection->ForceClose();
            }
            if (loop) loop->Shutdown();
            if (m_impl->m_loopThread.joinable() && m_impl->m_loopThread.get_id() != std::this_thread::get_id()) {
                m_impl->m_loopThread.join();
            }

            {
                std::lock_guard<std::mutex> lock(m_impl->m_stateMutex);
                for (auto& channel : m_impl->m_channels) {
                    if (channel && m_impl->m_loop) {
                        channel->DisableAll();
                        (void)m_impl->m_loop->UnregisterChannel(channel.get());
                    }
                }
                m_impl->m_channels.clear();

                if (!IsDatagramTransport(m_impl->m_transportKind)) {
                    for (SocketType fd : m_impl->m_listenFds) {
                        Internal::CloseSocket(fd);
                    }
                }
                m_impl->m_listenFds.clear();
                m_impl->m_datagramConnections.clear();
                m_impl->m_loop.reset();
                SetStatus(Status::Stopped);
            }
        }

        Server::Status Server::GetStatus() const noexcept {
            return m_impl ? m_impl->m_status.load(std::memory_order_acquire) : Status::Stopped;
        }

        std::vector<Address> Server::GetListenAddresses() const {
            if (m_impl == nullptr) return {};

            std::lock_guard<std::mutex> lock(m_impl->m_stateMutex); // 保护绑定地址快照
            return m_impl->m_boundAddresses;
        }

        void Server::Listen() {
            if (m_impl == nullptr) return;

            m_impl->m_boundAddresses.clear();
            for (const Address& address : m_impl->m_requestedAddresses) {
                if (!address.IsValid()) continue;

                const int socketType = SocketTypeForTransport(m_impl->m_transportKind); // 当前传输族 socket 类型
                const int protocol = SocketProtocolForTransport(m_impl->m_transportKind); // 当前传输族协议号
                SocketType fd = Internal::CreateSocket(address.FamilyValue(), socketType, protocol);
                if (fd == kInvalidSocket) continue;

                (void)Internal::SetReuseAddress(fd);
                if (::bind(fd, address.SockAddr(), address.Length()) != 0) {
                    Internal::CloseSocket(fd);
                    continue;
                }

                if (!IsDatagramTransport(m_impl->m_transportKind)) {
                    if (::listen(fd, SOMAXCONN) != 0) {
                        Internal::CloseSocket(fd);
                        continue;
                    }
                }

                (void)Internal::SetNonBlocking(fd, true);
                m_impl->m_listenFds.push_back(fd);
                m_impl->m_boundAddresses.push_back(Address::GetLocalAddress(fd));
            }

            if (m_impl->m_listenFds.empty()) {
                SetStatus(Status::Stopped);
                throw std::runtime_error("Server failed to listen on any address");
            }
        }

        void Server::AcceptReady(SocketType listenFd) {
            if (m_impl == nullptr) return;

            for (;;) {
                sockaddr_storage storage{}; // accept 输出的远端地址
                SocketLength length = static_cast<SocketLength>(sizeof(storage));
                SocketType clientFd = ::accept(listenFd, reinterpret_cast<sockaddr*>(&storage), &length);
                if (clientFd == kInvalidSocket) {
                    const int error = Internal::GetLastSocketError();
                    if (Internal::IsInterrupted(error)) continue;
                    if (Internal::IsWouldBlock(error)) return;
                    return;
                }

                (void)Internal::SetNonBlocking(clientFd, true);
                (void)Internal::SetTcpNoDelay(clientFd);

                std::shared_ptr<Connection> connection; // 新接受的 TCP 连接
                if (m_impl->m_factory) {
                    connection = m_impl->m_factory.Create(clientFd, m_impl->m_loop.get());
                }
                else {
                    connection = CreateDefaultServerConnection(clientFd, m_impl->m_loop.get(), m_impl->m_transportKind);
                }

                if (!connection) {
                    // factory 返回空时丢弃该连接，避免泄漏已接受 socket。
                    Internal::CloseSocket(clientFd);
                    continue;
                }

                connection->SetFrameworkCloseCallback([this](Connection& closed) {
                    if (m_impl && m_impl->m_loop) m_impl->m_loop->DetachConnection(closed.GetSocket());
                });
                m_impl->m_loop->AttachConnection(connection);
                connection->Start();
            }
        }

        void Server::AttachDatagramConnection(SocketType fd) {
            if (m_impl == nullptr) return;

            std::shared_ptr<Connection> connection; // 绑定到 UDP socket 的长期连接对象
            if (m_impl->m_factory) {
                connection = m_impl->m_factory.Create(fd, m_impl->m_loop.get());
            }
            else {
                connection = CreateDefaultServerConnection(fd, m_impl->m_loop.get(), m_impl->m_transportKind);
            }

            if (!connection) {
                // factory 返回空时关闭绑定 socket，避免事件循环持有不可用传输。
                Internal::CloseSocket(fd);
                throw std::runtime_error("Server connection factory returned null for datagram transport");
            }

            connection->SetFrameworkCloseCallback([this](Connection& closed) {
                if (m_impl && m_impl->m_loop) m_impl->m_loop->DetachConnection(closed.GetSocket());
            });
            m_impl->m_loop->AttachConnection(connection);
            connection->Start();
            m_impl->m_datagramConnections.push_back(std::move(connection));
        }

        void Server::SetStatus(Status status) {
            if (m_impl == nullptr) return;

            m_impl->m_status.store(status, std::memory_order_release);
            m_impl->m_stateCv.notify_all();
        }
    }
}
