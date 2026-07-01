#include <LikesProgram/Net/Client.hpp>
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
                // TCP 使用流 socket，UDP 使用数据报 socket。
                return IsDatagramTransport(kind) ? SOCK_DGRAM : SOCK_STREAM;
            }

            int SocketProtocolForTransport(TransportKind kind) {
                // 协议号与 socket 类型保持一致，避免 Windows 下创建失败。
                return IsDatagramTransport(kind) ? IPPROTO_UDP : IPPROTO_TCP;
            }

            std::shared_ptr<Connection> CreateDefaultClientConnection(
                SocketType fd,
                EventLoop* loop,
                TransportKind kind) {
                // Net 内置明文 TCP/UDP；安全层由用户 factory 注入自定义派生 transport。
                if (kind == TransportKind::Tcp) {
                    return std::make_shared<Connection>(fd, loop, std::make_unique<TcpTransport>(fd));
                }

                if (kind == TransportKind::Udp) {
                    return std::make_shared<Connection>(fd, loop, std::make_unique<UdpTransport>(fd));
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
            std::thread m_loopThread;                      // EventLoop 线程
            std::atomic<Status> m_status{ Status::Stopped }; // 客户端生命周期状态
            mutable std::mutex m_stateMutex;               // 保护状态切换与连接指针
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

            std::lock_guard<std::mutex> lock(m_impl->m_stateMutex); // 串行化启动/停止状态
            if (m_impl->m_status.load(std::memory_order_acquire) != Status::Stopped) return;

            SetStatus(Status::Connecting);
            if (!m_impl->m_factory.InitializeSharedSecureResources()) {
                SetStatus(Status::Stopped);
                return;
            }

            const int socketType = SocketTypeForTransport(m_impl->m_transportKind); // 当前传输族需要的 socket 类型
            const int protocol = SocketProtocolForTransport(m_impl->m_transportKind); // 当前传输族需要的协议号
            SocketType fd = Internal::CreateSocket(m_impl->m_remoteAddress.FamilyValue(), socketType, protocol);
            if (fd == kInvalidSocket) {
                SetStatus(Status::Stopped);
                return;
            }

            // UDP connect 只绑定默认对端，仍然保留 datagram 语义。
            if (::connect(fd, m_impl->m_remoteAddress.SockAddr(), m_impl->m_remoteAddress.Length()) != 0) {
                Internal::CloseSocket(fd);
                SetStatus(Status::Stopped);
                return;
            }

            (void)Internal::SetNonBlocking(fd, true);
            if (!IsDatagramTransport(m_impl->m_transportKind)) {
                (void)Internal::SetTcpNoDelay(fd);
            }

            m_impl->m_loop = std::make_shared<EventLoop>();
            if (m_impl->m_factory) {
                m_impl->m_connection = m_impl->m_factory.Create(fd, m_impl->m_loop.get());
            }
            else {
                m_impl->m_connection = CreateDefaultClientConnection(fd, m_impl->m_loop.get(), m_impl->m_transportKind);
            }

            if (!m_impl->m_connection) {
                // factory 返回空时关闭 socket，避免留下半初始化连接。
                Internal::CloseSocket(fd);
                m_impl->m_loop.reset();
                SetStatus(Status::Stopped);
                return;
            }

            m_impl->m_connection->SetFrameworkCloseCallback([this](Connection&) {
                SetStatus(Status::Stopped);
            });
            m_impl->m_loop->AttachConnection(m_impl->m_connection);
            m_impl->m_connection->Start();

            auto loop = m_impl->m_loop; // 后台线程持有事件循环生命周期
            m_impl->m_loopThread = std::thread([loop]() {
                loop->Start();
            });

            SetStatus(Status::Connected);
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

            std::shared_ptr<EventLoop> loop; // 当前事件循环快照
            std::shared_ptr<Connection> connection; // 当前连接快照
            {
                std::lock_guard<std::mutex> lock(m_impl->m_stateMutex);
                const Status status = m_impl->m_status.load(std::memory_order_acquire);
                const bool hasResources = m_impl->m_loop || m_impl->m_connection || m_impl->m_loopThread.joinable();
                if (status == Status::Stopped && !hasResources) return;
                SetStatus(Status::Stopping);
                loop = m_impl->m_loop;
                connection = m_impl->m_connection;
            }

            if (connection) connection->ForceClose();
            if (loop) loop->Shutdown();
            if (m_impl->m_loopThread.joinable() && m_impl->m_loopThread.get_id() != std::this_thread::get_id()) {
                m_impl->m_loopThread.join();
            }

            {
                std::lock_guard<std::mutex> lock(m_impl->m_stateMutex);
                m_impl->m_connection.reset();
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
