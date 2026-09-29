#include <LikesProgram/Net/Client.hpp>
#include <LikesProgram/Net/EventLoop.hpp>
#include "net/platform/SocketOps.hpp"
#include "net/TcpConnector.hpp"
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace LikesProgram {
    namespace Net {
        namespace {
            int SocketTypeForTransport(TransportKind kind) {
                // TCP 使用流 socket，UDP 使用数据报 socket。
                return IsDatagramTransport(kind) ? SOCK_DGRAM : SOCK_STREAM;
            }

            int SocketProtocolForTransport(TransportKind kind) {
                // 协议号与 socket 类型保持一致，避免 Windows 下创建失败。
                return IsDatagramTransport(kind) ? IPPROTO_UDP : IPPROTO_TCP;
            }

            SocketType CreateConnectedDatagramSocket(const Address& remoteAddress, TransportKind kind) {
                const int socketType = SocketTypeForTransport(kind); // UDP 仍按传输族创建数据报 socket。
                const int protocol = SocketProtocolForTransport(kind); // UDP 使用 IPPROTO_UDP，避免 Windows 下协议不匹配。
                SocketType fd = Internal::CreateSocket(remoteAddress.FamilyValue(), socketType, protocol);
                if (fd == kInvalidSocket) return kInvalidSocket;

                // UDP connect 只绑定默认对端，仍然保留 datagram 语义。
                if (Internal::ConnectSocket(fd, remoteAddress.SockAddr(), remoteAddress.Length()) != 0) {
                    Internal::CloseSocket(fd);
                    return kInvalidSocket;
                }

                (void)Internal::SetNonBlocking(fd, true);
                return fd;
            }

            struct ClientConnectRequest {
                EventLoop* m_loop = nullptr;                         // connect operation 所属 issuer loop
                Poller::ConnectId m_connectId = Poller::InvalidConnectId; // Shutdown 使用的取消标识
                int m_error = 0;                                    // CQE 或提交失败错误码
                bool m_finished = false;                            // connect 与 Connection::Start 已完成
                bool m_cancelled = false;                           // Shutdown 已撤销本次启动
                std::mutex m_mutex;                                 // 串行化 Start/Shutdown/CQE
                std::condition_variable m_cv;                       // 同步 Client::Start 等待 completion
            };

            void CancelClientConnectRequest(
                const std::shared_ptr<ClientConnectRequest>& request) noexcept {
                if (!request) return;
                EventLoop* loop = nullptr; // 取消必须投递回创建 operation 的 issuer
                Poller::ConnectId connectId = Poller::InvalidConnectId;
                {
                    std::lock_guard<std::mutex> lock(request->m_mutex);
                    request->m_cancelled = true;
                    loop = request->m_loop;
                    connectId = request->m_connectId;
                    request->m_cv.notify_all();
                }
                if (loop != nullptr && connectId != Poller::InvalidConnectId) {
                    loop->PostTask([loop, connectId]() {
                        Internal::TcpConnector::Cancel(loop, connectId);
                    });
                }
            }

            std::shared_ptr<Connection> CreateDefaultClientConnection(
                SocketType fd,
                EventLoop* loop,
                TransportKind kind) {
                // Net 内置明文 TCP/UDP；安全层由用户 factory 注入自定义 Engine。
                if (kind == TransportKind::Tcp) {
                    return std::make_shared<Connection>(fd, loop);
                }

                if (kind == TransportKind::Udp) {
                    return std::make_shared<Connection>(fd, loop, kind);
                }

                return {};
            }
        }

        struct Client::ClientImpl {
            Address m_remoteAddress;                       // 远端地址
            TransportKind m_transportKind = TransportKind::Tcp; // 客户端使用的传输族
            ConnectionFactory m_factory;                   // 连接工厂
            std::shared_ptr<EventLoop> m_loop;             // 客户端后台事件循环
            std::shared_ptr<Connection> m_connection;      // 当前连接
            std::shared_ptr<ClientConnectRequest> m_connectRequest; // 尚未交付的 TCP connect completion
            std::thread m_loopThread;                      // EventLoop 线程
            std::atomic<Status> m_status{ Status::Stopped }; // 客户端生命周期状态
            mutable std::mutex m_stateMutex;               // 保护状态切换与连接指针
            mutable std::mutex m_shutdownMutex;            // 串行化资源停止与 thread join
            mutable std::condition_variable m_stateCv;     // Shutdown 等待条件
        };

        Client::Client(const Address& remoteAddress, ConnectionFactory factory)
            : Client(remoteAddress, TransportKind::Tcp, std::move(factory)) {
        }

        Client::Client(const Address& remoteAddress, TransportKind transportKind, ConnectionFactory factory)
            : m_impl(new ClientImpl{}) {
            m_impl->m_remoteAddress = remoteAddress;
            m_impl->m_transportKind = transportKind;
            m_impl->m_factory = std::move(factory);
        }

        Client::~Client() {
            Shutdown();
            delete m_impl;
            m_impl = nullptr;
        }

        void Client::Start() {
            if (m_impl == nullptr) return;

            bool hasStoppedResources = false; // peer close 后状态可先于 loop/thread 资源收敛
            {
                std::lock_guard<std::mutex> lock(m_impl->m_stateMutex);
                if (m_impl->m_status.load(std::memory_order_acquire) != Status::Stopped) return;
                if (m_impl->m_loopThread.joinable()
                    && m_impl->m_loopThread.get_id() == std::this_thread::get_id()) {
                    return; // 旧 loop 回调栈内不能同步回收并重启同一 Client
                }
                hasStoppedResources = m_impl->m_loop
                    || m_impl->m_connection
                    || m_impl->m_connectRequest
                    || m_impl->m_loopThread.joinable();
            }
            if (hasStoppedResources) Shutdown(); // 新 Start 前停止旧 loop 并 join 已退出线程

            if (IsDatagramTransport(m_impl->m_transportKind)) {
                std::lock_guard<std::mutex> lock(m_impl->m_stateMutex); // UDP 兼容分支留待 datagram completion 重建
                if (m_impl->m_status.load(std::memory_order_acquire) != Status::Stopped) return;
                SetStatus(Status::Connecting);

                const SocketType fd = CreateConnectedDatagramSocket(
                    m_impl->m_remoteAddress,
                    m_impl->m_transportKind);
                if (fd == kInvalidSocket) {
                    SetStatus(Status::Stopped);
                    return;
                }

                m_impl->m_loop = std::make_shared<EventLoop>();
                m_impl->m_connection = m_impl->m_factory
                    ? m_impl->m_factory.Create(fd, m_impl->m_loop.get())
                    : CreateDefaultClientConnection(fd, m_impl->m_loop.get(), m_impl->m_transportKind);
                if (!m_impl->m_connection) {
                    Internal::CloseSocket(fd);
                    m_impl->m_loop.reset();
                    SetStatus(Status::Stopped);
                    return;
                }

                m_impl->m_connection->SetFrameworkCloseCallback([this](Connection&) {
                    SetStatus(Status::Stopped);
                });
                m_impl->m_loop->AttachConnection(m_impl->m_connection);
                auto loop = m_impl->m_loop; // 后台线程持有 UDP 兼容 EventLoop
                auto connection = m_impl->m_connection; // issuer 启动任务持有连接
                m_impl->m_loopThread = std::thread([loop]() { loop->Start(); });
                loop->PostTask([connection]() { connection->Start(); });
                SetStatus(Status::Connected);
                return;
            }

            std::shared_ptr<EventLoop> loop; // TCP completion 线程与 Start 共享 loop 生命周期
            std::shared_ptr<ClientConnectRequest> request;
            {
                std::lock_guard<std::mutex> lock(m_impl->m_stateMutex);
                if (m_impl->m_status.load(std::memory_order_acquire) != Status::Stopped) return;

                SetStatus(Status::Connecting);
                m_impl->m_loop = std::make_shared<EventLoop>();
                loop = m_impl->m_loop;
                request = std::make_shared<ClientConnectRequest>();
                request->m_loop = loop.get();
                m_impl->m_connectRequest = request;
            }

            loop->PostTask([this, loop, request]() {
                {
                    std::lock_guard<std::mutex> lock(request->m_mutex);
                    if (request->m_cancelled) {
                        request->m_finished = true;
                        request->m_cv.notify_all();
                        return;
                    }
                }

                const Poller::ConnectId connectId = Internal::TcpConnector::Start(
                    loop.get(),
                    m_impl->m_remoteAddress,
                    [this, loop, request](SocketType fd, int error) {
                        {
                            std::lock_guard<std::mutex> lock(request->m_mutex);
                            if (request->m_cancelled) {
                                Internal::CloseSocket(fd);
                                request->m_finished = true;
                                request->m_cv.notify_all();
                                return;
                            }
                        }

                        std::shared_ptr<Connection> connection; // CQE issuer 线程完成 factory 与启动
                        if (error == 0 && fd != kInvalidSocket) {
                            try {
                                connection = m_impl->m_factory
                                    ? m_impl->m_factory.Create(fd, loop.get())
                                    : CreateDefaultClientConnection(fd, loop.get(), TransportKind::Tcp);
                                if (!connection) throw std::runtime_error("Client factory returned an empty connection");

                                connection->SetFrameworkCloseCallback([this](Connection&) {
                                    SetStatus(Status::Stopped);
                                });
                                loop->AttachConnection(connection);
                                connection->Start();
                                if (!connection->IsConnected()) {
                                    throw std::runtime_error("Client completion connection failed to start");
                                }
                            }
                            catch (...) {
                                if (connection) connection->ForceClose();
                                else Internal::CloseSocket(fd);
                                connection.reset();
                                error = EIO;
                            }
                        }

                        bool accepted = false; // 只有仍处于 Connecting 的同一启动轮次可以提交连接
                        if (connection) {
                            std::lock_guard<std::mutex> lock(m_impl->m_stateMutex);
                            accepted = m_impl->m_status.load(std::memory_order_acquire) == Status::Connecting
                                && m_impl->m_connectRequest == request;
                            if (accepted) {
                                m_impl->m_connection = connection;
                                SetStatus(Status::Connected);
                            }
                        }
                        if (connection && !accepted) connection->ForceClose();

                        std::lock_guard<std::mutex> lock(request->m_mutex);
                        request->m_error = accepted ? 0 : (error != 0 ? error : ECANCELED);
                        request->m_finished = true;
                        request->m_cv.notify_all();
                    });

                bool cancelNow = false; // Shutdown 可能在 StartConnect 返回前撤销请求
                {
                    std::lock_guard<std::mutex> lock(request->m_mutex);
                    request->m_connectId = connectId;
                    cancelNow = request->m_cancelled;
                    if (connectId == Poller::InvalidConnectId) {
                        request->m_error = EIO;
                        request->m_finished = true;
                        request->m_cv.notify_all();
                    }
                }
                if (cancelNow && connectId != Poller::InvalidConnectId) {
                    Internal::TcpConnector::Cancel(loop.get(), connectId);
                }
            });

            {
                std::lock_guard<std::mutex> lock(m_impl->m_stateMutex);
                if (m_impl->m_status.load(std::memory_order_acquire) != Status::Connecting
                    || m_impl->m_connectRequest != request) {
                    return;
                }
                // connect 任务先入队，Activate 失败时也能运行失败通知并唤醒同步 Start。
                m_impl->m_loopThread = std::thread([loop]() { loop->Start(); });
            }

            int connectError = 0; // Start 保持同步 API，但等待的是 Poller completion 而非阻塞 connect syscall
            bool cancelled = false;
            {
                std::unique_lock<std::mutex> lock(request->m_mutex);
                request->m_cv.wait(lock, [&request]() {
                    return request->m_finished || request->m_cancelled;
                });
                connectError = request->m_error;
                cancelled = request->m_cancelled;
            }
            if (connectError == 0 && !cancelled) return;

            bool shouldCleanup = false; // 并发 Shutdown 已接管资源时不重复 join 同一线程
            {
                std::lock_guard<std::mutex> lock(m_impl->m_stateMutex);
                shouldCleanup = m_impl->m_status.load(std::memory_order_acquire) == Status::Connecting;
            }
            if (shouldCleanup) Shutdown();
        }

        void Client::WaitShutdown() const noexcept {
            if (m_impl == nullptr) return;

            std::unique_lock<std::mutex> lock(m_impl->m_stateMutex); // 等待状态回到 Stopped
            m_impl->m_stateCv.wait(lock, [this]() {
                return m_impl->m_status.load(std::memory_order_acquire) == Status::Stopped;
            });
        }

        void Client::Shutdown() {
            if (m_impl == nullptr) return;

            std::lock_guard<std::mutex> shutdownLock(m_impl->m_shutdownMutex); // 防止并发重复 join/reset

            std::shared_ptr<EventLoop> loop; // 当前事件循环快照
            std::shared_ptr<Connection> connection; // 当前连接快照
            std::shared_ptr<ClientConnectRequest> connectRequest; // 可能尚在内核中的 connect operation
            {
                std::lock_guard<std::mutex> lock(m_impl->m_stateMutex);
                const Status status = m_impl->m_status.load(std::memory_order_acquire);
                const bool hasResources = m_impl->m_loop || m_impl->m_connection || m_impl->m_loopThread.joinable();
                if (status == Status::Stopped && !hasResources) return;
                SetStatus(Status::Stopping);
                loop = m_impl->m_loop;
                connection = m_impl->m_connection;
                connectRequest = m_impl->m_connectRequest;
            }

            CancelClientConnectRequest(connectRequest);
            if (connection) connection->ForceClose();
            if (loop) loop->Shutdown();
            if (m_impl->m_loopThread.joinable() && m_impl->m_loopThread.get_id() != std::this_thread::get_id()) {
                m_impl->m_loopThread.join();
            }

            {
                std::lock_guard<std::mutex> lock(m_impl->m_stateMutex);
                m_impl->m_connection.reset();
                m_impl->m_connectRequest.reset();
                m_impl->m_loop.reset();
                SetStatus(Status::Stopped);
            }
        }

        Client::Status Client::GetStatus() const noexcept {
            return m_impl ? m_impl->m_status.load(std::memory_order_acquire) : Status::Stopped;
        }

        std::shared_ptr<Connection> Client::GetConnection() const {
            if (m_impl == nullptr) return {};

            std::lock_guard<std::mutex> lock(m_impl->m_stateMutex); // 保护连接快照
            return m_impl->m_connection;
        }

        void Client::SetStatus(Status status) {
            if (m_impl == nullptr) return;

            m_impl->m_status.store(status, std::memory_order_release);
            m_impl->m_stateCv.notify_all();
        }
    }
}
