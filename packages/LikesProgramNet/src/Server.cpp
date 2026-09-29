#include <LikesProgram/Net/Server.hpp>
#include <LikesProgram/Net/Connection.hpp>
#include <LikesProgram/Net/EventLoop.hpp>
#include <LikesProgram/Net/EventLoopGroup.hpp>
#include "net/platform/SocketOps.hpp"
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
                // Net 内置 TCP/UDP 明文传输；安全层可由用户派生 Engine 后通过 factory 注入。
                if (kind == TransportKind::Tcp) {
                    return std::make_shared<Connection>(fd, loop);
                }

                if (kind == TransportKind::Udp) {
                    return std::make_shared<Connection>(fd, loop, kind);
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
            std::unique_ptr<EventLoopGroup> m_workerGroup;     // worker Reactor 组，未配置时为空
            std::size_t m_workerThreadCount = 0;               // Start 前配置的 worker 数量
            std::vector<SocketType> m_listenFds;               // 监听 socket 集合
            std::vector<EventLoop*> m_listenerLoops;           // 与监听 socket 一一对应的 owner loop
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

            m_impl->m_loop = std::make_shared<EventLoop>();
            if (m_impl->m_workerThreadCount > 0) {
                m_impl->m_workerGroup = std::make_unique<EventLoopGroup>(m_impl->m_workerThreadCount);
                m_impl->m_workerGroup->Start();
            }
            Listen();

            for (std::size_t index = 0; index < m_impl->m_listenFds.size(); ++index) {
                const SocketType fd = m_impl->m_listenFds[index]; // 当前 listener fd
                if (IsDatagramTransport(m_impl->m_transportKind)) {
                    // UDP 没有 accept 阶段，一个绑定 socket 对应一个长期连接对象。
                    AttachDatagramConnection(fd);
                    continue;
                }

                EventLoop* ownerLoop = m_impl->m_listenerLoops[index]; // worker-local listener 所属 ring
                ownerLoop->PostTask([this, fd, ownerLoop]() {
                    // listener 直接提交 multishot accept，不再经过 readiness Channel 与 accept drain。
                    (void)ownerLoop->StartPollerAccept(fd, [this, ownerLoop](SocketType clientFd) {
                        EventLoop* targetLoop = ownerLoop; // 默认沿用单 loop 或 worker-local listener
                        if (m_impl != nullptr
                            && m_impl->m_workerGroup
                            && m_impl->m_workerGroup->IsRunning()
                            && !Internal::SupportsWorkerLocalListenerReuse()) {
                            // Windows 单 listener issuer 只负责把 fd 投递给目标 worker。
                            targetLoop = PickConnectionLoop();
                        }
                        if (targetLoop == nullptr) {
                            Internal::CloseSocket(clientFd);
                            return;
                        }
                        if (targetLoop != ownerLoop || !targetLoop->IsInLoopThread()) {
                            try {
                                targetLoop->PostTask([this, clientFd, targetLoop]() {
                                    HandleAccepted(clientFd, targetLoop);
                                });
                            }
                            catch (...) {
                                // 投递失败时仍由 accept completion 边界归还 socket。
                                Internal::CloseSocket(clientFd);
                            }
                            return;
                        }
                        HandleAccepted(clientFd, targetLoop);
                    });
                });
            }

            if (!m_impl->m_workerGroup) {
                auto loop = m_impl->m_loop; // 单 loop 模式由后台线程持有事件循环生命周期
                m_impl->m_loopThread = std::thread([loop]() {
                    loop->Start();
                });
            }

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
            EventLoopGroup* workerGroup = nullptr; // worker 组快照，停止时仍由 ServerImpl 持有
            std::vector<std::shared_ptr<Connection>> datagramConnections; // UDP 连接快照
            {
                std::lock_guard<std::mutex> lock(m_impl->m_stateMutex);
                const Status status = m_impl->m_status.load(std::memory_order_acquire);
                if (status == Status::Stopped) return;
                SetStatus(Status::Stopping);
                loop = m_impl->m_loop;
                workerGroup = m_impl->m_workerGroup.get();
                datagramConnections = m_impl->m_datagramConnections;
            }

            for (const auto& connection : datagramConnections) {
                if (connection) connection->ForceClose();
            }
            if (!IsDatagramTransport(m_impl->m_transportKind)) {
                // accept 取消与 listener close 必须在所属 issuer 线程按顺序执行。
                for (std::size_t index = 0; index < m_impl->m_listenFds.size(); ++index) {
                    const SocketType fd = m_impl->m_listenFds[index]; // 当前待停 listener
                    EventLoop* ownerLoop = m_impl->m_listenerLoops[index]; // 对应 single issuer loop
                    if (ownerLoop == nullptr) {
                        Internal::CloseSocket(fd);
                        continue;
                    }
                    ownerLoop->PostTask([ownerLoop, fd]() {
                        ownerLoop->StopPollerAccept(fd);
                        Internal::CloseSocket(fd);
                    });
                }
            }
            if (loop) loop->Shutdown();
            if (m_impl->m_loopThread.joinable() && m_impl->m_loopThread.get_id() != std::this_thread::get_id()) {
                m_impl->m_loopThread.join();
            }
            if (workerGroup) workerGroup->Shutdown();

            {
                std::lock_guard<std::mutex> lock(m_impl->m_stateMutex);
                m_impl->m_listenFds.clear();
                m_impl->m_listenerLoops.clear();
                m_impl->m_datagramConnections.clear();
                m_impl->m_workerGroup.reset();
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

        void Server::SetWorkerThreads(std::size_t workerCount) {
            if (m_impl == nullptr) return;

            std::lock_guard<std::mutex> lock(m_impl->m_stateMutex); // worker 数量只允许启动前稳定配置
            if (m_impl->m_status.load(std::memory_order_acquire) != Status::Stopped) return;
            m_impl->m_workerThreadCount = workerCount;
        }

        void Server::Listen() {
            if (m_impl == nullptr) return;

            m_impl->m_boundAddresses.clear();
            m_impl->m_listenFds.clear();
            m_impl->m_listenerLoops.clear();
            for (const Address& address : m_impl->m_requestedAddresses) {
                if (!address.IsValid()) continue;

                const int socketType = SocketTypeForTransport(m_impl->m_transportKind); // 当前传输族 socket 类型
                const int protocol = SocketProtocolForTransport(m_impl->m_transportKind); // 当前传输族协议号
                const bool workerLocalTcp = !IsDatagramTransport(m_impl->m_transportKind)
                    && m_impl->m_workerGroup
                    && m_impl->m_workerGroup->IsRunning()
                    && Internal::SupportsWorkerLocalListenerReuse(); // TCP worker 各自持有可复用 listener
                const std::size_t listenerCount = workerLocalTcp
                    ? m_impl->m_workerGroup->Size()
                    : 1;
                Address sharedAddress = address; // port 0 由首个 listener 解析后给其余 worker 复用
                bool addressBound = false; // 每个用户地址只对外报告一次实际绑定地址

                for (std::size_t listenerIndex = 0; listenerIndex < listenerCount; ++listenerIndex) {
                    SocketType fd = Internal::CreateSocket(address.FamilyValue(), socketType, protocol);
                    if (fd == kInvalidSocket) continue;

                    const bool reuseReady = Internal::SetReuseAddress(fd)
                        && (!workerLocalTcp || Internal::SetReusePort(fd));
                    if (!reuseReady || Internal::BindSocket(
                        fd,
                        sharedAddress.SockAddr(),
                        sharedAddress.Length()) != 0) {
                        Internal::CloseSocket(fd);
                        continue;
                    }

                    if (!IsDatagramTransport(m_impl->m_transportKind)
                        && Internal::ListenSocket(fd, SOMAXCONN) != 0) {
                        Internal::CloseSocket(fd);
                        continue;
                    }

                    (void)Internal::SetNonBlocking(fd, true);
                    const Address actualAddress = Address::GetLocalAddress(fd); // 首个 port 0 绑定后的真实端口
                    if (!addressBound) {
                        sharedAddress = actualAddress;
                        m_impl->m_boundAddresses.push_back(actualAddress);
                        addressBound = true;
                    }
                    EventLoop* listenerLoop = workerLocalTcp
                        ? m_impl->m_workerGroup->NextLoop()
                        : (m_impl->m_workerGroup && m_impl->m_workerGroup->IsRunning()
                            ? m_impl->m_workerGroup->NextLoop()
                            : m_impl->m_loop.get()); // 当前 listener 的唯一 accept issuer
                    m_impl->m_listenFds.push_back(fd);
                    m_impl->m_listenerLoops.push_back(listenerLoop);
                }
            }

            if (m_impl->m_listenFds.empty()) {
                SetStatus(Status::Stopped);
                throw std::runtime_error("Server failed to listen on any address");
            }
        }

        void Server::HandleAccepted(SocketType clientFd, EventLoop* ownerLoop) {
            if (m_impl == nullptr || ownerLoop == nullptr || clientFd == kInvalidSocket) {
                if (clientFd != kInvalidSocket) Internal::CloseSocket(clientFd);
                return;
            }
            const Status status = m_impl->m_status.load(std::memory_order_acquire); // accept CQE 到达时的生命周期
            if (status == Status::Stopping || status == Status::Stopped) {
                // shutdown task 尚未消费 cancel CQE 时，丢弃当前批次内的晚到连接。
                Internal::CloseSocket(clientFd);
                return;
            }

            (void)Internal::SetNonBlocking(clientFd, true);
            (void)Internal::SetTcpNoDelay(clientFd);

            std::shared_ptr<Connection> connection; // 新接受的 TCP 连接
            if (m_impl->m_factory) {
                connection = m_impl->m_factory.Create(clientFd, ownerLoop);
            }
            else {
                connection = CreateDefaultServerConnection(clientFd, ownerLoop, m_impl->m_transportKind);
            }

            if (!connection) {
                // factory 返回空时丢弃该连接，避免泄漏已接受 socket。
                Internal::CloseSocket(clientFd);
                return;
            }
            const Status afterFactory = m_impl->m_status.load(std::memory_order_acquire); // 用户 factory 可触发 Shutdown
            if (afterFactory == Status::Stopping || afterFactory == Status::Stopped) {
                // 尚未挂入 EventLoop，局部 shared_ptr 析构会按 Connection 所有权关闭 socket。
                return;
            }
            AttachConnectionToLoop(connection, ownerLoop);
        }

        void Server::AttachDatagramConnection(SocketType fd) {
            if (m_impl == nullptr) return;

            EventLoop* ownerLoop = PickConnectionLoop(); // UDP 绑定 socket 也归属一个固定 loop
            std::shared_ptr<Connection> connection; // 绑定到 UDP socket 的长期连接对象
            if (m_impl->m_factory) {
                connection = m_impl->m_factory.Create(fd, ownerLoop);
            }
            else {
                connection = CreateDefaultServerConnection(fd, ownerLoop, m_impl->m_transportKind);
            }

            if (!connection) {
                // factory 返回空时关闭绑定 socket，避免事件循环持有不可用传输。
                Internal::CloseSocket(fd);
                throw std::runtime_error("Server connection factory returned null for datagram transport");
            }

            AttachConnectionToLoop(connection, ownerLoop);
            m_impl->m_datagramConnections.push_back(std::move(connection));
        }

        EventLoop* Server::PickConnectionLoop() noexcept {
            if (m_impl == nullptr) return nullptr;

            if (m_impl->m_workerGroup && m_impl->m_workerGroup->IsRunning()) {
                if (EventLoop* worker = m_impl->m_workerGroup->NextLoop()) return worker;
            }
            return m_impl->m_loop.get();
        }

        void Server::AttachConnectionToLoop(const std::shared_ptr<Connection>& connection, EventLoop* ownerLoop) {
            if (m_impl == nullptr || !connection || ownerLoop == nullptr) return;

            // Connection::DoClose 统一在关闭 socket 前从 owner loop 摘除，避免 fd 复用竞态。
            ownerLoop->AttachConnection(connection);

            if (ownerLoop->IsInLoopThread()) {
                try {
                    connection->Start();
                }
                catch (...) {
                    // Connection 已接管 accepted fd，统一关闭后不得再把异常交给 Poller 重复 close。
                    connection->ForceClose();
                }
                return;
            }

            ownerLoop->PostTask([connection]() {
                try {
                    connection->Start();
                }
                catch (...) {
                    // 异步启动异常不能越过 EventLoop 任务边界终止 worker。
                    connection->ForceClose();
                }
            });
        }

        void Server::SetStatus(Status status) {
            if (m_impl == nullptr) return;

            m_impl->m_status.store(status, std::memory_order_release);
            m_impl->m_stateCv.notify_all();
        }
    }
}
