#include <LikesProgram/Net/ConnectionPool.hpp>
#include <LikesProgram/Net/EventLoop.hpp>
#include <LikesProgram/Net/EventLoopGroup.hpp>
#include "net/ConnectionIdentityPolicy.hpp"
#include "net/platform/SocketOps.hpp"
#include "net/TcpConnector.hpp"
#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace LikesProgram {
    namespace Net {
        namespace Internal {
            class ConnectionPoolState;
        }

        struct ConnectionLease::ConnectionLeaseImpl {
            std::shared_ptr<Internal::ConnectionPoolState> m_state; // 池状态，允许租约长于 ConnectionPool 对象
            std::shared_ptr<Connection> m_connection;               // 当前借出的连接
        };

        namespace Internal {
            class ConnectionPoolState : public std::enable_shared_from_this<ConnectionPoolState> {
            public:
                struct ConnectRequest {
                    std::shared_ptr<EventLoop> m_loop;                   // 延长 connect operation 所属 issuer loop
                    Poller::ConnectId m_connectId = Poller::InvalidConnectId; // 已提交 operation 的取消标识
                    std::shared_ptr<Connection> m_connection;           // 成功完成并启动的连接
                    std::exception_ptr m_exception;                     // factory/启动异常，回到 Acquire 线程重抛
                    int m_error = 0;                                    // connect CQE 或提交失败错误码
                    bool m_finished = false;                            // callback 已给出最终结果
                    bool m_cancelled = false;                           // timeout/Shutdown 已撤销等待
                    std::mutex m_mutex;                                 // 串行化 issuer callback 与等待线程
                    std::condition_variable m_cv;                       // 唤醒同步 Acquire API
                };

                explicit ConnectionPoolState(const ConnectionPoolOptions& options)
                    : m_remoteAddress(options.remoteAddress),
                    m_factory(options.factory),
                    m_maxConnections(options.maxConnections),
                    m_maxIdleConnections(std::min(options.maxIdleConnections, options.maxConnections)),
                    m_workerThreads(std::max<std::size_t>(options.workerThreads, 1)),
                    m_acquireTimeout(options.acquireTimeout) {
                    if (options.transportKind != TransportKind::Tcp) {
                        throw std::invalid_argument("ConnectionPool only supports TCP outbound connections");
                    }
                    if (!m_remoteAddress.IsValid()) {
                        throw std::invalid_argument("ConnectionPool remote address is invalid");
                    }
                    if (m_maxConnections == 0) {
                        throw std::invalid_argument("ConnectionPool maxConnections must be greater than zero");
                    }
                }

                void Start() {
                    m_group = std::make_shared<EventLoopGroup>(m_workerThreads);
                    m_group->Start();
                }

                ConnectionLease Acquire(std::chrono::milliseconds timeout) {
                    const auto deadline = std::chrono::steady_clock::now() + timeout;
                    bool reservedCreate = false; // 已为本次建连预留 active/pending 名额

                    while (true) {
                        std::unique_lock<std::mutex> lock(m_mutex);
                        if (m_shutdown) return {};

                        PurgeClosedIdleLocked();
                        if (!m_idleConnections.empty()) {
                            auto connection = std::move(m_idleConnections.back());
                            m_idleConnections.pop_back();
                            ++m_activeConnections;
                            return MakeLease(std::move(connection));
                        }

                        if (m_totalConnections + m_pendingCreates < m_maxConnections) {
                            ++m_pendingCreates;
                            ++m_activeConnections;
                            reservedCreate = true;
                            break;
                        }

                        ++m_waitingAcquires;
                        const bool notified = m_cv.wait_until(lock, deadline, [this]() {
                            return m_shutdown
                                || !m_idleConnections.empty()
                                || m_totalConnections + m_pendingCreates < m_maxConnections;
                        });
                        --m_waitingAcquires;

                        if (!notified && std::chrono::steady_clock::now() >= deadline) return {};
                    }

                    try {
                        auto connection = CreateConnection(deadline);
                        std::lock_guard<std::mutex> lock(m_mutex);
                        --m_pendingCreates;

                        if (!connection || m_shutdown
                            || !IsConnectionLeaseable(
                                connection->IsConnected(),
                                connection->GetSocket() != kInvalidSocket)) {
                            --m_activeConnections;
                            if (connection) CloseConnection(std::move(connection));
                            m_cv.notify_all();
                            return {};
                        }

                        RegisterConnectionLocked(connection);
                        if (!IsConnectionLeaseable(
                            connection->IsConnected(),
                            connection->GetSocket() != kInvalidSocket)) {
                            RemoveKnownConnectionLocked(connection.get()); // 只回滚当前对象，不按复用 fd 删除
                            --m_activeConnections;
                            CloseConnection(std::move(connection));
                            m_cv.notify_all();
                            return {};
                        }
                        reservedCreate = false;
                        m_cv.notify_all();
                        return MakeLease(std::move(connection));
                    }
                    catch (...) {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        if (reservedCreate) {
                            --m_pendingCreates;
                            --m_activeConnections;
                            m_cv.notify_all();
                        }
                        throw;
                    }
                }

                void Release(std::shared_ptr<Connection> connection, bool reusable) noexcept {
                    if (!connection) return;

                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        if (m_activeConnections > 0) --m_activeConnections;

                        const bool known = m_connections.find(connection.get()) != m_connections.end();
                        const bool canReuse = reusable
                            && !m_shutdown
                            && known
                            && connection->IsConnected()
                            && m_idleConnections.size() < m_maxIdleConnections;

                        if (canReuse) {
                            m_idleConnections.push_back(std::move(connection));
                        }
                        else {
                            RemoveKnownConnectionLocked(connection.get());
                            if (!m_shutdown) CloseConnection(connection);
                        }

                        m_cv.notify_all();
                    }
                }

                void OnConnectionClosed(Connection* connection) noexcept {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    const auto registered = m_connections.find(connection); // 对象地址是池内权威身份
                    if (registered == m_connections.end()
                        || !MatchesWeakIdentity(registered->second, connection)) {
                        return;
                    }
                    RemoveKnownConnectionLocked(connection);
                    PurgeClosedIdleLocked();
                    m_cv.notify_all();
                }

                void Shutdown() noexcept {
                    std::vector<std::shared_ptr<Connection>> connections;
                    std::vector<std::shared_ptr<ConnectRequest>> connectRequests;
                    std::shared_ptr<EventLoopGroup> group;
                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        if (m_shutdown && !m_group) return;

                        m_shutdown = true;
                        for (auto& item : m_connections) {
                            if (auto connection = item.second.lock()) connections.push_back(std::move(connection));
                        }
                        m_idleConnections.clear();
                        m_connections.clear();
                        m_totalConnections = 0;
                        connectRequests.assign(m_connectRequests.begin(), m_connectRequests.end());
                        m_connectRequests.clear();
                        group = std::move(m_group);
                        m_cv.notify_all();
                    }

                    for (const auto& request : connectRequests) CancelConnectRequest(request);
                    for (auto& connection : connections) {
                        CloseConnection(std::move(connection));
                    }
                    if (group) group->Shutdown();
                }

                ConnectionPoolStats Stats() const {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    ConnectionPoolStats stats{};
                    stats.activeConnections = m_activeConnections;
                    stats.idleConnections = m_idleConnections.size();
                    stats.totalConnections = m_totalConnections + m_pendingCreates;
                    stats.waitingAcquires = m_waitingAcquires;
                    return stats;
                }

                std::chrono::milliseconds AcquireTimeout() const noexcept {
                    return m_acquireTimeout;
                }

            private:
                ConnectionLease MakeLease(std::shared_ptr<Connection> connection) {
                    return ConnectionLease(new ConnectionLease::ConnectionLeaseImpl{
                        shared_from_this(),
                        std::move(connection)
                    });
                }

                std::shared_ptr<Connection> CreateConnection(
                    std::chrono::steady_clock::time_point deadline) {
                    std::shared_ptr<EventLoopGroup> group;
                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        group = m_group;
                    }

                    if (!group) throw std::runtime_error("ConnectionPool is shut down");

                    std::shared_ptr<EventLoop> loop = group->NextLoopShared();
                    if (!loop) throw std::runtime_error("ConnectionPool has no worker EventLoop");

                    auto request = std::make_shared<ConnectRequest>(); // 等待线程与 issuer callback 共享一次尝试
                    request->m_loop = loop;
                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        if (m_shutdown) return {};
                        m_connectRequests.insert(request);
                    }

                    auto state = shared_from_this(); // 提交任务结束前保持 factory 与池回调状态
                    loop->PostTask([state, request]() {
                        state->SubmitConnectRequest(request);
                    });

                    bool timedOut = false; // deadline 同时覆盖连接名额等待与内核 connect
                    {
                        std::unique_lock<std::mutex> lock(request->m_mutex);
                        if (!request->m_cv.wait_until(lock, deadline, [&request]() {
                            return request->m_finished || request->m_cancelled;
                        })) {
                            request->m_cancelled = true;
                            timedOut = true;
                        }
                    }
                    if (timedOut) CancelConnectRequest(request);

                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        m_connectRequests.erase(request);
                    }
                    std::lock_guard<std::mutex> requestLock(request->m_mutex);
                    if (request->m_exception) std::rethrow_exception(request->m_exception);
                    if (request->m_error != 0) {
                        throw std::runtime_error("ConnectionPool TCP connect completion failed");
                    }
                    return request->m_cancelled ? std::shared_ptr<Connection>{} : request->m_connection;
                }

                void SubmitConnectRequest(const std::shared_ptr<ConnectRequest>& request) noexcept {
                    if (!request) return;
                    {
                        std::lock_guard<std::mutex> lock(request->m_mutex);
                        if (request->m_cancelled) {
                            request->m_finished = true;
                            request->m_cv.notify_all();
                            return;
                        }
                    }

                    std::weak_ptr<ConnectionPoolState> weakState = shared_from_this(); // CQE 不强制延长已关闭池
                    const Poller::ConnectId connectId = Internal::TcpConnector::Start(
                        request->m_loop.get(),
                        m_remoteAddress,
                        [weakState, request](SocketType fd, int error) {
                            if (auto state = weakState.lock()) {
                                state->CompleteConnectRequest(request, fd, error);
                            }
                            else {
                                Internal::CloseSocket(fd);
                            }
                        });

                    bool cancelNow = false; // timeout 可能与 Start 返回并发交错
                    {
                        std::lock_guard<std::mutex> lock(request->m_mutex);
                        request->m_connectId = connectId;
                        cancelNow = request->m_cancelled;
                        if (connectId == Poller::InvalidConnectId) {
                            request->m_error = EIO; // 具体平台错误仍由 Poller 诊断口保留
                            request->m_finished = true;
                            request->m_cv.notify_all();
                        }
                    }
                    if (cancelNow && connectId != Poller::InvalidConnectId) {
                        Internal::TcpConnector::Cancel(request->m_loop.get(), connectId);
                    }
                }

                void CompleteConnectRequest(
                    const std::shared_ptr<ConnectRequest>& request,
                    SocketType fd,
                    int error) noexcept {
                    if (!request) {
                        Internal::CloseSocket(fd);
                        return;
                    }
                    {
                        std::lock_guard<std::mutex> lock(request->m_mutex);
                        if (request->m_cancelled) {
                            Internal::CloseSocket(fd);
                            request->m_finished = true;
                            request->m_cv.notify_all();
                            return;
                        }
                    }
                    if (error != 0 || fd == kInvalidSocket) {
                        std::lock_guard<std::mutex> lock(request->m_mutex);
                        request->m_error = error != 0 ? error : EIO;
                        request->m_finished = true;
                        request->m_cv.notify_all();
                        return;
                    }

                    std::shared_ptr<Connection> connection; // callback 成功后在 issuer 线程完成对象装配
                    try {
                        connection = m_factory
                                ? m_factory.Create(fd, request->m_loop.get())
                                : std::make_shared<Connection>(fd, request->m_loop.get());
                        if (!connection) throw std::runtime_error("ConnectionPool factory returned an empty connection");

                        std::weak_ptr<ConnectionPoolState> weakState = shared_from_this();
                        connection->SetFrameworkCloseCallback([weakState](Connection& closed) {
                            if (auto state = weakState.lock()) {
                                state->OnConnectionClosed(&closed);
                            }
                        });
                        request->m_loop->AttachConnection(connection);
                        connection->Start();
                        if (!connection->IsConnected()) {
                            throw std::runtime_error("ConnectionPool completion connection failed to start");
                        }
                    }
                    catch (...) {
                        if (connection) connection->ForceClose();
                        else Internal::CloseSocket(fd);
                        std::lock_guard<std::mutex> lock(request->m_mutex);
                        request->m_exception = std::current_exception();
                        request->m_finished = true;
                        request->m_cv.notify_all();
                        return;
                    }

                    bool cancelled = false; // Shutdown 可在连接启动后、结果交付前撤销租约
                    {
                        std::lock_guard<std::mutex> lock(request->m_mutex);
                        cancelled = request->m_cancelled;
                        if (!cancelled) request->m_connection = connection;
                        request->m_finished = true;
                        request->m_cv.notify_all();
                    }
                    if (cancelled) CloseConnection(std::move(connection));
                }

                static void CancelConnectRequest(const std::shared_ptr<ConnectRequest>& request) noexcept {
                    if (!request) return;
                    std::shared_ptr<EventLoop> loop; // 取消任务持有原 issuer loop 到任务消费
                    Poller::ConnectId connectId = Poller::InvalidConnectId;
                    {
                        std::lock_guard<std::mutex> lock(request->m_mutex);
                        request->m_cancelled = true;
                        loop = request->m_loop;
                        connectId = request->m_connectId;
                        request->m_cv.notify_all();
                    }
                    if (loop && connectId != Poller::InvalidConnectId) {
                        loop->PostTask([loop, connectId]() {
                            Internal::TcpConnector::Cancel(loop.get(), connectId);
                        });
                    }
                }

                void RegisterConnectionLocked(const std::shared_ptr<Connection>& connection) {
                    if (!connection || connection->GetSocket() == kInvalidSocket) return;

                    const auto inserted = m_connections.emplace(connection.get(), connection); // 指针身份不受 fd 复用影响
                    if (inserted.second) ++m_totalConnections;
                    else inserted.first->second = connection;
                }

                void RemoveKnownConnectionLocked(Connection* connection) noexcept {
                    if (connection == nullptr) return;
                    const auto registered = m_connections.find(connection); // stale 回调不得删除其他对象
                    if (registered == m_connections.end()
                        || !MatchesWeakIdentity(registered->second, connection)) {
                        return;
                    }
                    m_connections.erase(registered);
                    if (m_totalConnections > 0) --m_totalConnections;
                    m_idleConnections.erase(
                        std::remove_if(
                            m_idleConnections.begin(),
                            m_idleConnections.end(),
                            [connection](const std::shared_ptr<Connection>& candidate) {
                                return !candidate || candidate.get() == connection;
                            }),
                        m_idleConnections.end());
                }

                void PurgeClosedIdleLocked() noexcept {
                    std::vector<std::shared_ptr<Connection>> closedConnections; // 删除 weak 索引前延长对象身份
                    m_idleConnections.erase(
                        std::remove_if(
                            m_idleConnections.begin(),
                            m_idleConnections.end(),
                            [&closedConnections](const std::shared_ptr<Connection>& connection) {
                                if (connection && connection->IsConnected()) return false;
                                if (connection) closedConnections.push_back(connection);
                                return true;
                            }),
                        m_idleConnections.end());

                    for (const auto& connection : closedConnections) {
                        RemoveKnownConnectionLocked(connection.get());
                    }
                }

                static void CloseConnection(std::shared_ptr<Connection> connection) noexcept {
                    if (connection) connection->ForceClose();
                }

                Address m_remoteAddress;                                      // TCP 远端地址
                ConnectionFactory m_factory;                                  // 用户工厂或空工厂
                std::size_t m_maxConnections = 0;                             // 最大连接名额
                std::size_t m_maxIdleConnections = 0;                         // 最大空闲连接
                std::size_t m_workerThreads = 1;                              // worker EventLoop 数
                std::chrono::milliseconds m_acquireTimeout{ 1000 };           // 默认获取超时
                std::shared_ptr<EventLoopGroup> m_group;                      // outbound I/O worker 组
                std::vector<std::shared_ptr<Connection>> m_idleConnections;   // 空闲复用连接
                std::unordered_set<std::shared_ptr<ConnectRequest>> m_connectRequests; // 尚未交付的 completion connect
                std::unordered_map<Connection*, std::weak_ptr<Connection>> m_connections; // 按对象身份保存所有连接
                std::size_t m_activeConnections = 0;                          // 已借出或建连中租约数
                std::size_t m_totalConnections = 0;                           // 已注册连接数
                std::size_t m_pendingCreates = 0;                             // 建连中保留名额
                std::size_t m_waitingAcquires = 0;                            // 等待者数量
                bool m_shutdown = false;                                      // 池关闭标记
                mutable std::mutex m_mutex;                                   // 保护池状态
                std::condition_variable m_cv;                                 // 等待可用连接/名额
            };
        }

        ConnectionLease::ConnectionLease() = default;

        ConnectionLease::ConnectionLease(ConnectionLeaseImpl* impl)
            : m_impl(impl) {
        }

        ConnectionLease::ConnectionLease(ConnectionLease&& other) noexcept
            : m_impl(other.m_impl) {
            other.m_impl = nullptr;
        }

        ConnectionLease::~ConnectionLease() {
            ReleaseCurrent(true);
        }

        ConnectionLease& ConnectionLease::operator=(ConnectionLease&& other) noexcept {
            if (this == &other) return *this;
            ReleaseCurrent(true);
            m_impl = other.m_impl;
            other.m_impl = nullptr;
            return *this;
        }

        std::shared_ptr<Connection> ConnectionLease::Get() const {
            return m_impl ? m_impl->m_connection : std::shared_ptr<Connection>{};
        }

        Connection* ConnectionLease::operator->() const noexcept {
            return m_impl ? m_impl->m_connection.get() : nullptr;
        }

        ConnectionLease::operator bool() const noexcept {
            return m_impl != nullptr && static_cast<bool>(m_impl->m_connection);
        }

        void ConnectionLease::Release() {
            ReleaseCurrent(true);
        }

        void ConnectionLease::Discard() {
            ReleaseCurrent(false);
        }

        void ConnectionLease::ReleaseCurrent(bool reusable) noexcept {
            ConnectionLeaseImpl* impl = m_impl;
            m_impl = nullptr;
            if (impl == nullptr) return;

            auto state = std::move(impl->m_state);
            auto connection = std::move(impl->m_connection);
            delete impl;

            if (state) state->Release(std::move(connection), reusable);
        }

        struct ConnectionPool::ConnectionPoolImpl {
            std::shared_ptr<Internal::ConnectionPoolState> m_state; // 池共享状态
        };

        ConnectionPool::ConnectionPool(const ConnectionPoolOptions& options) {
            auto impl = std::make_unique<ConnectionPoolImpl>(); // 构造失败前由局部智能指针保持异常安全
            impl->m_state = std::make_shared<Internal::ConnectionPoolState>(options);
            impl->m_state->Start();
            m_impl = impl.release(); // 全部可能抛出的初始化完成后再提交 PImpl 所有权
        }

        ConnectionPool::~ConnectionPool() {
            Shutdown();
            delete m_impl;
            m_impl = nullptr;
        }

        ConnectionLease ConnectionPool::Acquire() {
            if (m_impl == nullptr || !m_impl->m_state) return {};

            ConnectionLease lease = m_impl->m_state->Acquire(m_impl->m_state->AcquireTimeout());
            if (!lease) throw std::runtime_error("ConnectionPool acquire timed out");
            return lease;
        }

        ConnectionLease ConnectionPool::TryAcquire(std::chrono::milliseconds timeout) {
            if (m_impl == nullptr || !m_impl->m_state) return {};
            return m_impl->m_state->Acquire(timeout);
        }

        void ConnectionPool::Shutdown() {
            if (m_impl != nullptr && m_impl->m_state) m_impl->m_state->Shutdown();
        }

        ConnectionPoolStats ConnectionPool::Stats() const {
            if (m_impl == nullptr || !m_impl->m_state) return {};
            return m_impl->m_state->Stats();
        }
    }
}
