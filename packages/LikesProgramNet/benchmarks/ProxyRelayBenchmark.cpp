#include "ProxyRelayBenchmark.hpp"

#include <LikesProgram/Net/Client.hpp>
#include <LikesProgram/Net/Connection.hpp>
#include <LikesProgram/Net/EventLoop.hpp>
#include <LikesProgram/Net/Server.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace LikesProgram {
    namespace Net {
        namespace Benchmarks {
            namespace {
                using Clock = std::chrono::steady_clock;

                struct ProxyRelayCounters {
                    std::atomic<std::uint64_t> ingressToUpstreamBytes{ 0 }; // ingress -> upstream
                    std::atomic<std::uint64_t> upstreamToIngressBytes{ 0 }; // upstream -> ingress
                    std::atomic<std::uint64_t> backendReceivedBytes{ 0 }; // backend 消费字节
                    std::atomic<std::uint64_t> clientReceivedBytes{ 0 }; // load client 消费字节
                    std::atomic<std::size_t> earlyQueuePeakBytes{ 0 }; // 早到队列峰值
                    std::atomic<std::uint64_t> ingressHighWatermarkEvents{ 0 }; // ingress high
                    std::atomic<std::uint64_t> ingressLowWatermarkEvents{ 0 }; // ingress low
                    std::atomic<std::uint64_t> upstreamHighWatermarkEvents{ 0 }; // upstream high
                    std::atomic<std::uint64_t> upstreamLowWatermarkEvents{ 0 }; // upstream low
                    std::atomic<std::uint64_t> overflowEvents{ 0 }; // early/write overflow
                    std::atomic<std::uint64_t> errors{ 0 }; // I/O/连接错误
                    std::atomic<bool> loadSendSubmitted{ false }; // load 独立发送已提交
                    std::atomic<bool> backendSendSubmitted{ false }; // backend 独立发送已提交
                    std::atomic<bool> fullDuplexOverlapObserved{ false }; // 双向同时在途窗口
                    std::atomic<bool> delayWorkerLocked{ false }; // worker 已提升 weak session
                    std::atomic<bool> delayWorkerExited{ true }; // 未启动 worker 等价于已退出
                    std::atomic<std::uint64_t> delayWorkerJoinEvents{ 0 }; // 外层真实 join 次数
                    std::atomic<std::uint64_t> ownerStopAttempts{ 0 }; // owner 进入 stop 闸门次数
                    std::atomic<std::uint64_t> expiredHandoffDrops{ 0 }; // 已失效 handoff 丢弃次数
                    std::atomic<bool> remoteStopRequested{ false }; // 非 owner 已抢 stop 闸门
                    std::atomic<bool> releaseRemoteStop{ false }; // 允许非 owner 继续投递
                    std::atomic<bool> upstreamHandoffWaiting{ false }; // upstream handoff 已进入屏障
                    std::atomic<bool> releaseUpstreamHandoff{ false }; // 允许 handoff 投递 owner
                    std::atomic<std::uint64_t> upstreamClientOwnerStopEvents{ 0 }; // owner 停止次数
                    std::atomic<std::uint64_t> upstreamClientWrongThreadStops{ 0 }; // 错误线程停止次数
                    std::atomic<bool> resourcesConverged{ false }; // Run 统一资源屏障
                    std::atomic<bool> cleanShutdown{ false }; // 是否完成稳定收敛
                    mutable std::mutex reasonMutex; // 关闭原因串行化
                    std::string closeReason; // 首个关闭原因
                };

                void RecordCloseReason(
                    const std::shared_ptr<ProxyRelayCounters>& counters,
                    const char* reason) {
                    std::lock_guard<std::mutex> lock(counters->reasonMutex); // 只保留第一个稳定原因
                    if (counters->closeReason.empty()) counters->closeReason = reason;
                }

                void UpdatePeak(std::atomic<std::size_t>& peak, std::size_t value) {
                    std::size_t observed = peak.load(std::memory_order_relaxed); // 原子更新队列峰值
                    while (observed < value
                        && !peak.compare_exchange_weak(
                            observed,
                            value,
                            std::memory_order_release,
                            std::memory_order_relaxed)) {
                    }
                }

                void MarkIndependentSendSubmitted(
                    const std::shared_ptr<ProxyRelayCounters>& counters,
                    bool fromLoad) {
                    auto& current = fromLoad
                        ? counters->loadSendSubmitted
                        : counters->backendSendSubmitted; // 当前方向提交标志
                    const auto& opposite = fromLoad
                        ? counters->backendSendSubmitted
                        : counters->loadSendSubmitted; // 对向提交标志
                    current.store(true, std::memory_order_release);
                    if (opposite.load(std::memory_order_acquire)
                        && counters->backendReceivedBytes.load(std::memory_order_acquire) == 0
                        && counters->clientReceivedBytes.load(std::memory_order_acquire) == 0) {
                        counters->fullDuplexOverlapObserved.store(true, std::memory_order_release);
                    }
                }

                template <typename Predicate>
                bool WaitUntil(Predicate predicate, std::chrono::milliseconds timeout) {
                    const auto deadline = Clock::now() + timeout; // 每轮使用明确超时边界
                    while (!predicate() && Clock::now() < deadline) std::this_thread::yield();
                    return predicate();
                }

                std::shared_ptr<Connection> WaitForClientConnection(Client& client) {
                    std::shared_ptr<Connection> connection; // 异步 connect 的连接快照
                    const bool connected = WaitUntil(
                        [&client, &connection]() {
                            connection = client.GetConnection();
                            return static_cast<bool>(connection);
                        },
                        std::chrono::seconds(2));
                    return connected ? connection : nullptr;
                }

                class RelaySession;

                struct RelayControl {
                    std::mutex mutex; // benchmark 主线程与 ingress owner 同步 session
                    std::weak_ptr<RelaySession> session; // 控制面不延长 session 生命周期
                    std::shared_ptr<std::thread> delayWorker; // 外层持有并 join 延迟 worker
                };

                class RelayIngressConnection final
                    : public Connection,
                      public std::enable_shared_from_this<RelayIngressConnection> {
                public:
                    RelayIngressConnection(
                        SocketType fd,
                        EventLoop* loop,
                        Address upstreamAddress,
                        ProxyRelayOptions options,
                        std::shared_ptr<ProxyRelayCounters> counters,
                        std::shared_ptr<RelayControl> control)
                        : Connection(fd, loop),
                          m_upstreamAddress(std::move(upstreamAddress)),
                          m_options(std::move(options)),
                          m_counters(std::move(counters)),
                          m_control(std::move(control)) {
                        SetWriteWatermark(
                            m_options.ingressWriteHighWatermarkBytes,
                            m_options.ingressWriteLowWatermarkBytes);
                        SetMaxPendingWriteBytes(m_options.ingressMaxPendingWriteBytes);
                    }

                    void SetSession(const std::shared_ptr<RelaySession>& session) {
                        m_session = session; // ingress 只强持有 session，session 反向使用 weak_ptr
                    }

                protected:
                    void OnConnected() override;
                    void OnMessage(Buffer& input) override;
                    void OnWriteHighWatermark(std::size_t pendingBytes) override;
                    void OnWriteLowWatermark(std::size_t pendingBytes) override;
                    void OnWriteQueueOverflow(std::size_t pendingBytes) override;
                    void OnClosed() override;
                    void OnError(int error) override;

                private:
                    Address m_upstreamAddress; // backend 监听地址
                    ProxyRelayOptions m_options; // 本轮私有 benchmark 参数
                    std::shared_ptr<ProxyRelayCounters> m_counters; // 跨 worker 计数
                    std::shared_ptr<RelayControl> m_control; // benchmark 主动 Shutdown 控制面
                    std::shared_ptr<RelaySession> m_session; // ingress 生命周期拥有 session
                };

                class RelayUpstreamConnection final
                    : public Connection,
                      public std::enable_shared_from_this<RelayUpstreamConnection> {
                public:
                    RelayUpstreamConnection(
                        SocketType fd,
                        EventLoop* loop,
                        std::weak_ptr<RelaySession> session,
                        const ProxyRelayOptions& options)
                        : Connection(fd, loop),
                          m_session(std::move(session)) {
                        SetWriteWatermark(
                            options.upstreamWriteHighWatermarkBytes,
                            options.upstreamWriteLowWatermarkBytes);
                        SetMaxPendingWriteBytes(options.upstreamMaxPendingWriteBytes);
                    }

                protected:
                    void OnConnected() override;
                    void OnMessage(Buffer& input) override;
                    void OnWriteHighWatermark(std::size_t pendingBytes) override;
                    void OnWriteLowWatermark(std::size_t pendingBytes) override;
                    void OnWriteQueueOverflow(std::size_t pendingBytes) override;
                    void OnClosed() override;
                    void OnError(int error) override;

                private:
                    std::weak_ptr<RelaySession> m_session; // 回调不延长 session 生命周期
                };

                class RelaySession final : public std::enable_shared_from_this<RelaySession> {
                public:
                    RelaySession(
                        std::weak_ptr<RelayIngressConnection> ingress,
                        Address upstreamAddress,
                        ProxyRelayOptions options,
                        std::shared_ptr<ProxyRelayCounters> counters,
                        std::shared_ptr<RelayControl> control)
                        : m_ingress(std::move(ingress)),
                          m_upstreamAddress(std::move(upstreamAddress)),
                          m_options(std::move(options)),
                          m_counters(std::move(counters)),
                          m_control(std::move(control)) {
                    }

                    ~RelaySession() {
                        m_startCancelled.store(true, std::memory_order_release); // 禁止延迟启动线程再投递任务
                    }

                    void Start() {
                        m_ownerThreadId = std::this_thread::get_id(); // ingress OnConnected 所在线程为唯一 owner
                        {
                            std::lock_guard<std::mutex> lock(m_control->mutex);
                            m_control->session = shared_from_this(); // owner 建立后、同步 connect 前发布控制面
                        }
                        if (m_options.upstreamStartDelay <= std::chrono::milliseconds::zero()) {
                            StartUpstreamNow();
                            return;
                        }
                        const std::weak_ptr<RelaySession> weakSelf = shared_from_this();
                        const auto delay = m_options.upstreamStartDelay; // 线程不提前强持有 session
                        const auto holdAfterLock = m_options.delayWorkerHoldAfterLock; // 确定性竞态窗口
                        const auto counters = m_counters; // worker 退出指标独立于 session 生命周期
                        counters->delayWorkerExited.store(false, std::memory_order_release);
                        auto worker = std::make_shared<std::thread>(
                            [weakSelf, delay, holdAfterLock, counters]() {
                                std::this_thread::sleep_for(delay);
                                auto self = weakSelf.lock();
                                if (self) {
                                    counters->delayWorkerLocked.store(true, std::memory_order_release);
                                    std::this_thread::sleep_for(holdAfterLock);
                                    if (!self->m_startCancelled.load(std::memory_order_acquire)) {
                                        auto ingress = self->m_ingress.lock();
                                        if (ingress) {
                                            ingress->QueueInLoop([weakSelf]() {
                                                if (auto session = weakSelf.lock()) session->StartUpstreamNow();
                                            });
                                        }
                                    }
                                }
                                self.reset(); // 允许最后引用在 worker 上释放
                            });
                        std::lock_guard<std::mutex> lock(m_control->mutex); // 外层屏障接管 thread 句柄
                        m_control->delayWorker = std::move(worker);
                    }

                    void OnIngressMessage(Buffer& input) {
                        if (m_closed.load(std::memory_order_acquire)) {
                            input.RetrieveAll();
                            return;
                        }
                        const std::size_t bytes = input.ReadableBytes();
                        std::shared_ptr<RelayUpstreamConnection> upstream; // 锁内取得稳定连接快照
                        bool overflow = false; // 锁外统一触发关闭，避免回调重入状态锁
                        {
                            std::lock_guard<std::mutex> lock(m_stateMutex);
                            upstream = m_upstream;
                            if (!upstream || !upstream->IsConnected()) {
                                if (m_earlyQueueBytes + bytes > m_options.earlyQueueLimitBytes) {
                                    m_counters->overflowEvents.fetch_add(1, std::memory_order_relaxed);
                                    input.RetrieveAll();
                                    overflow = true;
                                } else {
                                    m_earlyQueueBytes += bytes;
                                    UpdatePeak(m_counters->earlyQueuePeakBytes, m_earlyQueueBytes);
                                    m_earlyQueue.emplace_back(std::move(input));
                                    return;
                                }
                            }
                        }
                        if (overflow) RequestStop("early_queue_overflow");
                        else upstream->Send(std::move(input));
                    }

                    void PostUpstreamConnected(const std::shared_ptr<RelayUpstreamConnection>& upstream) {
                        if (m_options.holdUpstreamBeforeOwnerQueue) {
                            m_counters->upstreamHandoffWaiting.store(true, std::memory_order_release);
                            while (!m_counters->releaseUpstreamHandoff.load(std::memory_order_acquire)) {
                                std::this_thread::yield(); // 保留 Client 尚拥有 Connection 的 handoff 窗口
                            }
                        }
                        auto ingress = m_ingress.lock();
                        if (!ingress) {
                            upstream->ForceClose();
                            return;
                        }
                        const std::weak_ptr<RelaySession> weakSelf = shared_from_this();
                        const std::weak_ptr<RelayUpstreamConnection> weakUpstream = upstream;
                        const auto counters = m_counters; // handoff 失效后仍可记录安全丢弃
                        ingress->QueueInLoop([weakSelf, weakUpstream, counters]() {
                            auto connection = weakUpstream.lock();
                            if (!connection) {
                                counters->expiredHandoffDrops.fetch_add(1, std::memory_order_relaxed);
                                return;
                            }
                            if (auto session = weakSelf.lock()) {
                                session->OnUpstreamConnectedOnOwner(connection);
                            } else {
                                // Client owner 负责关闭 Connection；此处不得触碰可能失效的 loop。
                                counters->expiredHandoffDrops.fetch_add(1, std::memory_order_relaxed);
                            }
                        });
                    }

                    void OnUpstreamConnectedOnOwner(const std::shared_ptr<RelayUpstreamConnection>& upstream) {
                        if (m_closed.load(std::memory_order_acquire)) {
                            upstream->ForceClose();
                            return;
                        }
                        std::vector<Buffer> queued;
                        {
                            std::lock_guard<std::mutex> lock(m_stateMutex);
                            m_upstream = upstream;
                            queued = std::move(m_earlyQueue);
                            m_earlyQueue.clear();
                            m_earlyQueueBytes = 0;
                        }
                        for (auto& buffer : queued) upstream->Send(std::move(buffer));
                    }

                    void OnUpstreamMessage(Buffer& input) {
                        if (m_closed.load(std::memory_order_acquire)) {
                            input.RetrieveAll();
                            return;
                        }
                        auto ingress = m_ingress.lock();
                        if (ingress && ingress->IsConnected()) {
                            ingress->Send(std::move(input));
                        } else {
                            input.RetrieveAll();
                        }
                    }

                    void OnIngressHigh() {
                        m_counters->ingressHighWatermarkEvents.fetch_add(1, std::memory_order_relaxed);
                        std::shared_ptr<RelayUpstreamConnection> upstream;
                        {
                            std::lock_guard<std::mutex> lock(m_stateMutex);
                            upstream = m_upstream;
                        }
                        if (upstream) upstream->PauseReading();
                    }

                    void OnIngressLow() {
                        m_counters->ingressLowWatermarkEvents.fetch_add(1, std::memory_order_relaxed);
                        std::shared_ptr<RelayUpstreamConnection> upstream;
                        {
                            std::lock_guard<std::mutex> lock(m_stateMutex);
                            upstream = m_upstream;
                        }
                        if (upstream) upstream->ResumeReading();
                    }

                    void OnUpstreamHigh() {
                        m_counters->upstreamHighWatermarkEvents.fetch_add(1, std::memory_order_relaxed);
                        if (auto ingress = m_ingress.lock()) ingress->PauseReading();
                    }

                    void OnUpstreamLow() {
                        m_counters->upstreamLowWatermarkEvents.fetch_add(1, std::memory_order_relaxed);
                        if (auto ingress = m_ingress.lock()) ingress->ResumeReading();
                    }

                    void OnIngressOverflow() {
                        m_counters->overflowEvents.fetch_add(1, std::memory_order_relaxed);
                        RequestStop("ingress_write_overflow");
                    }

                    void OnUpstreamOverflow() {
                        m_counters->overflowEvents.fetch_add(1, std::memory_order_relaxed);
                        RequestStop("upstream_write_overflow");
                    }

                    void OnIngressClosed() { RequestStop("ingress_eof"); }

                    void OnUpstreamClosed() { RequestStop("upstream_eof"); }

                    void OnError(const char* reason) {
                        m_counters->errors.fetch_add(1, std::memory_order_relaxed);
                        RequestStop(reason);
                    }

                    void RequestStop(const char* reason) {
                        const bool firstRequest = !m_stopRequested.exchange(true, std::memory_order_acq_rel);
                        if (firstRequest) RecordCloseReason(m_counters, reason);
                        if (std::this_thread::get_id() == m_ownerThreadId) {
                            m_counters->ownerStopAttempts.fetch_add(1, std::memory_order_relaxed);
                            StopOnOwner();
                            return;
                        }
                        if (!firstRequest) return;
                        if (m_options.holdRemoteStopBeforePost) {
                            m_counters->remoteStopRequested.store(true, std::memory_order_release);
                            while (!m_counters->releaseRemoteStop.load(std::memory_order_acquire)) {
                                std::this_thread::yield(); // 测试窗口只阻塞非 owner 回调
                            }
                        }
                        auto ingress = m_ingress.lock();
                        if (!ingress) return;
                        const std::weak_ptr<RelaySession> weakSelf = shared_from_this();
                        ingress->QueueInLoop([weakSelf]() {
                            if (auto session = weakSelf.lock()) session->StopOnOwner();
                        });
                    }

                    void StopOnOwner() {
                        if (m_ownerStopped) return;
                        if (std::this_thread::get_id() != m_ownerThreadId) {
                            m_counters->upstreamClientWrongThreadStops.fetch_add(1, std::memory_order_relaxed);
                            return;
                        }
                        m_ownerStopped = true;
                        m_closed.store(true, std::memory_order_release);
                        m_startCancelled.store(true, std::memory_order_release);
                        std::shared_ptr<RelayUpstreamConnection> upstream;
                        {
                            std::lock_guard<std::mutex> lock(m_stateMutex);
                            m_earlyQueue.clear();
                            m_earlyQueueBytes = 0;
                            upstream = std::exchange(m_upstream, nullptr);
                        }
                        if (upstream) upstream->ForceClose();
                        auto ingress = m_ingress.lock();
                        if (ingress) ingress->ForceClose();
                        if (m_client) {
                            m_client->Shutdown();
                            m_client.reset();
                        }
                        m_counters->upstreamClientOwnerStopEvents.fetch_add(1, std::memory_order_release);
                    }

                private:
                    void StartUpstreamNow() {
                        if (m_closed.load(std::memory_order_acquire) || m_client) return;
                        const std::weak_ptr<RelaySession> weakSelf = shared_from_this();
                        try {
                            m_client = std::make_unique<Client>(
                                m_upstreamAddress,
                                TransportKind::Tcp,
                                [weakSelf, options = m_options](SocketType fd, EventLoop* loop) {
                                    return std::make_shared<RelayUpstreamConnection>(
                                        fd,
                                        loop,
                                        weakSelf,
                                        options);
                            });
                            m_client->Start();
                            if (m_client->GetStatus() != Client::Status::Connected) {
                                OnError("upstream_connect_failure");
                            }
                        }
                        catch (...) {
                            OnError("upstream_connect_failure");
                        }
                    }

                    std::weak_ptr<RelayIngressConnection> m_ingress; // 回调链不形成环
                    mutable std::mutex m_stateMutex; // ingress/upstream worker 共享状态
                    Address m_upstreamAddress; // backend 目标
                    ProxyRelayOptions m_options; // 队列与水位策略
                    std::shared_ptr<ProxyRelayCounters> m_counters; // 共享指标
                    std::shared_ptr<RelayControl> m_control; // 外层 join 与主动控制
                    std::unique_ptr<Client> m_client; // upstream Client 生命周期
                    std::shared_ptr<RelayUpstreamConnection> m_upstream; // upstream Connection 快照
                    std::vector<Buffer> m_earlyQueue; // 建连前有界队列
                    std::size_t m_earlyQueueBytes = 0; // 队列当前字节数
                    std::atomic<bool> m_closed{ false }; // 幂等终态闸门
                    std::atomic<bool> m_stopRequested{ false }; // 跨 loop 首次终止请求
                    bool m_ownerStopped = false; // ingress owner 串行终态
                    std::thread::id m_ownerThreadId; // m_client 的唯一访问线程
                    std::atomic<bool> m_startCancelled{ false }; // 延迟启动取消标记
                };

                void RelayIngressConnection::OnConnected() {
                    auto self = shared_from_this();
                    auto session = std::make_shared<RelaySession>(
                        self,
                        m_upstreamAddress,
                        m_options,
                        m_counters,
                        m_control);
                    SetSession(session);
                    session->Start(); // Start 内先建立 owner 身份并发布控制面
                }

                void RelayIngressConnection::OnMessage(Buffer& input) {
                    if (auto session = m_session) session->OnIngressMessage(input);
                    else input.RetrieveAll();
                }

                void RelayIngressConnection::OnWriteHighWatermark(std::size_t) {
                    if (auto session = m_session) session->OnIngressHigh();
                }

                void RelayIngressConnection::OnWriteLowWatermark(std::size_t) {
                    if (auto session = m_session) session->OnIngressLow();
                }

                void RelayIngressConnection::OnWriteQueueOverflow(std::size_t) {
                    if (auto session = m_session) session->OnIngressOverflow();
                }

                void RelayIngressConnection::OnClosed() {
                    auto session = m_session;
                    if (session) session->OnIngressClosed();
                    m_session.reset();
                }

                void RelayIngressConnection::OnError(int) {
                    if (auto session = m_session) session->OnError("ingress_error");
                }

                void RelayUpstreamConnection::OnConnected() {
                    if (auto session = m_session.lock()) session->PostUpstreamConnected(shared_from_this());
                }

                void RelayUpstreamConnection::OnMessage(Buffer& input) {
                    if (auto session = m_session.lock()) session->OnUpstreamMessage(input);
                    else input.RetrieveAll();
                }

                void RelayUpstreamConnection::OnWriteHighWatermark(std::size_t) {
                    if (auto session = m_session.lock()) session->OnUpstreamHigh();
                }

                void RelayUpstreamConnection::OnWriteLowWatermark(std::size_t) {
                    if (auto session = m_session.lock()) session->OnUpstreamLow();
                }

                void RelayUpstreamConnection::OnWriteQueueOverflow(std::size_t) {
                    if (auto session = m_session.lock()) session->OnUpstreamOverflow();
                }

                void RelayUpstreamConnection::OnClosed() {
                    if (auto session = m_session.lock()) session->OnUpstreamClosed();
                }

                void RelayUpstreamConnection::OnError(int) {
                    if (auto session = m_session.lock()) session->OnError("upstream_error");
                }

                class BackendDuplexConnection final : public Connection {
                public:
                    BackendDuplexConnection(
                        SocketType fd,
                        EventLoop* loop,
                        std::shared_ptr<ProxyRelayCounters> counters,
                        std::size_t payloadBytes,
                        ProxyRelayScenario scenario)
                        : Connection(fd, loop),
                          m_counters(std::move(counters)),
                          m_payload(payloadBytes, 'b'),
                          m_scenario(scenario) {
                    }

                protected:
                    void OnConnected() override {
                        // backend 不等待正向数据，独立提交反向 payload。
                        MarkIndependentSendSubmitted(m_counters, false);
                        Send(m_payload.data(), m_payload.size());
                        if (m_scenario == ProxyRelayScenario::UpstreamEof
                            || m_scenario == ProxyRelayScenario::ConcurrentClose) {
                            Shutdown();
                        }
                    }

                    void OnMessage(Buffer& input) override {
                        const std::size_t bytes = input.ReadableBytes(); // backend 实际接收字节
                        m_counters->backendReceivedBytes.fetch_add(bytes, std::memory_order_relaxed);
                        m_counters->ingressToUpstreamBytes.fetch_add(bytes, std::memory_order_relaxed);
                        input.RetrieveAll();
                    }

                private:
                    std::shared_ptr<ProxyRelayCounters> m_counters; // backend 统计共享状态
                    std::string m_payload; // OnConnected 独立反向发送 payload
                    ProxyRelayScenario m_scenario = ProxyRelayScenario::Normal; // backend 场景
                };

                class LoadClientConnection final : public Connection {
                public:
                    LoadClientConnection(
                        SocketType fd,
                        EventLoop* loop,
                        std::shared_ptr<ProxyRelayCounters> counters)
                        : Connection(fd, loop),
                          m_counters(std::move(counters)) {
                    }

                protected:
                    void OnMessage(Buffer& input) override {
                        const std::size_t bytes = input.ReadableBytes(); // load 实际接收字节
                        m_counters->clientReceivedBytes.fetch_add(bytes, std::memory_order_relaxed);
                        m_counters->upstreamToIngressBytes.fetch_add(bytes, std::memory_order_relaxed);
                        input.RetrieveAll();
                    }

                private:
                    std::shared_ptr<ProxyRelayCounters> m_counters; // load client 统计共享状态
                };

                ProxyRelayStats SnapshotStats(
                    const ProxyRelayCounters& counters,
                    const ProxyRelayOptions& options,
                    double seconds) {
                    ProxyRelayStats stats;
                    stats.seconds = seconds;
                    stats.ingressToUpstreamBytes = counters.ingressToUpstreamBytes.load();
                    stats.upstreamToIngressBytes = counters.upstreamToIngressBytes.load();
                    stats.backendReceivedBytes = counters.backendReceivedBytes.load();
                    stats.clientReceivedBytes = counters.clientReceivedBytes.load();
                    stats.earlyQueuePeakBytes = counters.earlyQueuePeakBytes.load();
                    stats.earlyQueueLimitBytes = options.earlyQueueLimitBytes;
                    stats.ingressHighWatermarkEvents = counters.ingressHighWatermarkEvents.load();
                    stats.ingressLowWatermarkEvents = counters.ingressLowWatermarkEvents.load();
                    stats.upstreamHighWatermarkEvents = counters.upstreamHighWatermarkEvents.load();
                    stats.upstreamLowWatermarkEvents = counters.upstreamLowWatermarkEvents.load();
                    stats.overflowEvents = counters.overflowEvents.load();
                    stats.errors = counters.errors.load();
                    stats.fullDuplexOverlapObserved = counters.fullDuplexOverlapObserved.load();
                    stats.delayWorkerLocked = counters.delayWorkerLocked.load();
                    stats.delayWorkerExited = counters.delayWorkerExited.load();
                    stats.delayWorkerJoinEvents = counters.delayWorkerJoinEvents.load();
                    stats.ownerStopAttempts = counters.ownerStopAttempts.load();
                    stats.expiredHandoffDrops = counters.expiredHandoffDrops.load();
                    stats.upstreamClientOwnerStopEvents = counters.upstreamClientOwnerStopEvents.load();
                    stats.upstreamClientWrongThreadStops = counters.upstreamClientWrongThreadStops.load();
                    stats.resourcesConverged = counters.resourcesConverged.load();
                    stats.cleanShutdown = counters.cleanShutdown.load();
                    {
                        std::lock_guard<std::mutex> lock(counters.reasonMutex);
                        stats.closeReason = counters.closeReason;
                    }
                    return stats;
                }
            }

            std::string FormatProxyRelayStats(const ProxyRelayStats& stats) {
                std::ostringstream output;
                output << "tcp_proxy_full_duplex_bytes_per_second"
                    << " seconds=" << stats.seconds
                    << " throughput="
                    << (static_cast<double>(stats.ingressToUpstreamBytes + stats.upstreamToIngressBytes)
                        / std::max(stats.seconds, 1e-12))
                    << " unit=bytes/s\n";
                output << "tcp_proxy_full_duplex_stats"
                    << " ingress_to_upstream_bytes=" << stats.ingressToUpstreamBytes
                    << " upstream_to_ingress_bytes=" << stats.upstreamToIngressBytes
                    << " backend_received_bytes=" << stats.backendReceivedBytes
                    << " client_received_bytes=" << stats.clientReceivedBytes
                    << " early_queue_peak_bytes=" << stats.earlyQueuePeakBytes
                    << " early_queue_limit_bytes=" << stats.earlyQueueLimitBytes
                    << " ingress_high_watermark_events=" << stats.ingressHighWatermarkEvents
                    << " ingress_low_watermark_events=" << stats.ingressLowWatermarkEvents
                    << " upstream_high_watermark_events=" << stats.upstreamHighWatermarkEvents
                    << " upstream_low_watermark_events=" << stats.upstreamLowWatermarkEvents
                    << " overflow_events=" << stats.overflowEvents
                    << " clean_shutdown=" << (stats.cleanShutdown ? 1 : 0)
                    << " close_reason=" << stats.closeReason
                    << " errors=" << stats.errors
                    << '\n';
                return output.str();
            }

            ProxyRelayStats RunTcpProxyRelayBenchmark(const ProxyRelayOptions& options) {
                auto counters = std::make_shared<ProxyRelayCounters>();
                auto control = std::make_shared<RelayControl>(); // 主动 relay Shutdown 的 session 控制面
                Server backend(
                    Address("127.0.0.1", 0),
                    TransportKind::Tcp,
                    [counters, payloadBytes = options.payloadBytes, scenario = options.scenario](
                        SocketType fd,
                        EventLoop* loop) {
                        return std::make_shared<BackendDuplexConnection>(
                            fd,
                            loop,
                            counters,
                            payloadBytes,
                            scenario);
                    });
                backend.SetWorkerThreads(2);
                backend.Start();
                const auto backendAddresses = backend.GetListenAddresses();
                if (backendAddresses.empty() || backendAddresses.front().Port() == 0) {
                    throw std::runtime_error("proxy backend did not bind");
                }
                if (options.scenario == ProxyRelayScenario::ConnectFailure) backend.Shutdown();

                Server relay(
                    Address("127.0.0.1", 0),
                    TransportKind::Tcp,
                    [backendAddress = backendAddresses.front(), options, counters, control](
                        SocketType fd,
                        EventLoop* loop) {
                        return std::make_shared<RelayIngressConnection>(
                            fd,
                            loop,
                            backendAddress,
                            options,
                            counters,
                            control);
                    });
                relay.SetWorkerThreads(2);
                relay.Start();
                const auto relayAddresses = relay.GetListenAddresses();
                if (relayAddresses.empty() || relayAddresses.front().Port() == 0) {
                    relay.Shutdown();
                    backend.Shutdown();
                    throw std::runtime_error("proxy relay did not bind");
                }

                Client load(
                    relayAddresses.front(),
                    TransportKind::Tcp,
                    [counters](SocketType fd, EventLoop* loop) {
                        return std::make_shared<LoadClientConnection>(fd, loop, counters);
                    });
                load.Start();
                auto loadConnection = WaitForClientConnection(load);
                if (!loadConnection) {
                    load.Shutdown();
                    relay.Shutdown();
                    backend.Shutdown();
                    throw std::runtime_error("proxy load client did not connect");
                }

                const std::string payload(options.payloadBytes, 'p'); // 固定 payload 便于双向字节核对
                const auto started = Clock::now();
                const auto shutdownOwned = [&load, &relay, &backend, &counters, &control, &options]() {
                    std::shared_ptr<RelaySession> session; // 外层停止前先把 Client 清理投递给 ingress owner
                    {
                        std::lock_guard<std::mutex> lock(control->mutex);
                        session = control->session.lock();
                    }
                    if (session) session->RequestStop("benchmark_shutdown");
                    const bool ownerStopped = WaitUntil(
                        [&counters]() {
                            return counters->upstreamClientOwnerStopEvents.load(std::memory_order_acquire) == 1;
                        },
                        options.timeout);
                    load.Shutdown();
                    relay.Shutdown();
                    backend.Shutdown();
                    std::shared_ptr<std::thread> delayWorker; // 主线程取得稳定 worker 句柄
                    {
                        std::lock_guard<std::mutex> lock(control->mutex);
                        delayWorker = control->delayWorker;
                    }
                    bool workerJoined = true; // 未启动延迟 worker 时无需 join
                    if (delayWorker && delayWorker->joinable()) {
                        workerJoined = delayWorker->get_id() != std::this_thread::get_id();
                        if (workerJoined) {
                            delayWorker->join();
                            counters->delayWorkerJoinEvents.fetch_add(1, std::memory_order_relaxed);
                            counters->delayWorkerExited.store(true, std::memory_order_release);
                        }
                    }
                    const bool converged = ownerStopped
                        && workerJoined
                        && load.GetStatus() == Client::Status::Stopped
                        && relay.GetStatus() == Server::Status::Stopped
                        && backend.GetStatus() == Server::Status::Stopped
                        && counters->upstreamClientOwnerStopEvents.load(std::memory_order_acquire) == 1
                        && counters->upstreamClientWrongThreadStops.load(std::memory_order_acquire) == 0;
                    counters->resourcesConverged.store(converged, std::memory_order_release);
                    counters->cleanShutdown.store(converged, std::memory_order_release);
                    return converged;
                };
                MarkIndependentSendSubmitted(counters, true);
                loadConnection->Send(payload.data(), payload.size());
                if (options.forceCloseAfterDelayWorkerLock) {
                    const bool locked = WaitUntil(
                        [&counters]() {
                            return counters->delayWorkerLocked.load(std::memory_order_acquire);
                        },
                        options.timeout);
                    if (!locked) throw std::runtime_error("proxy relay delay worker did not lock session");
                    loadConnection->ForceClose();
                    const bool closed = WaitUntil(
                        [&counters]() {
                            return counters->upstreamClientOwnerStopEvents.load(std::memory_order_acquire) == 1;
                        },
                        options.timeout);
                    const double elapsed = std::max(
                        1e-12,
                        std::chrono::duration<double>(Clock::now() - started).count());
                    const bool converged = shutdownOwned();
                    if (!closed || !converged) {
                        throw std::runtime_error("proxy relay locked worker close timed out");
                    }
                    return SnapshotStats(*counters, options, elapsed);
                }
                if (options.forceCloseBeforeUpstream) {
                    loadConnection->ForceClose();
                    const bool closed = WaitUntil(
                        [&counters]() {
                            return counters->upstreamClientOwnerStopEvents.load(std::memory_order_acquire) == 1;
                        },
                        options.timeout);
                    const double elapsed = std::max(
                        1e-12,
                        std::chrono::duration<double>(Clock::now() - started).count());
                    const bool converged = shutdownOwned();
                    if (!closed || !converged) throw std::runtime_error("proxy relay force close timed out");
                    return SnapshotStats(*counters, options, elapsed);
                }
                if (options.scenario == ProxyRelayScenario::ConcurrentClose) {
                    const bool remoteRequested = WaitUntil(
                        [&counters]() {
                            return counters->remoteStopRequested.load(std::memory_order_acquire);
                        },
                        options.timeout);
                    if (!remoteRequested) {
                        counters->releaseRemoteStop.store(true, std::memory_order_release);
                        throw std::runtime_error("proxy relay remote stop did not enter barrier");
                    }
                    loadConnection->ForceClose(); // owner close 必须越过已被抢占的 stop 标志
                    const bool ownerAttempted = WaitUntil(
                        [&counters]() {
                            return counters->ownerStopAttempts.load(std::memory_order_acquire) > 0;
                        },
                        options.timeout);
                    counters->releaseRemoteStop.store(true, std::memory_order_release);
                    const bool stopped = WaitUntil(
                        [&counters]() {
                            return counters->upstreamClientOwnerStopEvents.load(std::memory_order_acquire) == 1;
                        },
                        options.timeout);
                    const double elapsed = std::max(
                        1e-12,
                        std::chrono::duration<double>(Clock::now() - started).count());
                    const bool converged = shutdownOwned();
                    if (!ownerAttempted || !stopped || !converged) {
                        throw std::runtime_error("proxy relay concurrent close timed out");
                    }
                    return SnapshotStats(*counters, options, elapsed);
                }
                if (options.scenario == ProxyRelayScenario::HandoffClose) {
                    const bool handoffWaiting = WaitUntil(
                        [&counters]() {
                            return counters->upstreamHandoffWaiting.load(std::memory_order_acquire);
                        },
                        options.timeout);
                    std::shared_ptr<RelaySession> session; // 控制面先于 handoff 投递 owner stop
                    const bool registered = WaitUntil(
                        [&control, &session]() {
                            std::lock_guard<std::mutex> lock(control->mutex);
                            session = control->session.lock();
                            return static_cast<bool>(session);
                        },
                        options.timeout);
                    if (!handoffWaiting || !registered) {
                        counters->releaseUpstreamHandoff.store(true, std::memory_order_release);
                        throw std::runtime_error("proxy relay handoff barrier did not initialize");
                    }
                    session->RequestStop("handoff_close");
                    counters->releaseUpstreamHandoff.store(true, std::memory_order_release);
                    const bool stopped = WaitUntil(
                        [&counters]() {
                            return counters->upstreamClientOwnerStopEvents.load(std::memory_order_acquire) == 1
                                && counters->expiredHandoffDrops.load(std::memory_order_acquire) == 1;
                        },
                        options.timeout);
                    const double elapsed = std::max(
                        1e-12,
                        std::chrono::duration<double>(Clock::now() - started).count());
                    const bool converged = shutdownOwned();
                    if (!stopped || !converged) throw std::runtime_error("proxy relay handoff close timed out");
                    return SnapshotStats(*counters, options, elapsed);
                }
                if (options.scenario == ProxyRelayScenario::UpstreamEof) {
                    const bool stopped = WaitUntil(
                        [&counters]() {
                            return counters->upstreamClientOwnerStopEvents.load(std::memory_order_acquire) == 1;
                        },
                        options.timeout);
                    const double elapsed = std::max(
                        1e-12,
                        std::chrono::duration<double>(Clock::now() - started).count());
                    const bool converged = shutdownOwned();
                    if (!stopped || !converged) throw std::runtime_error("proxy relay upstream EOF timed out");
                    return SnapshotStats(*counters, options, elapsed);
                }
                if (options.scenario == ProxyRelayScenario::RelayShutdown) {
                    std::shared_ptr<RelaySession> session; // 主线程只发请求，不触碰 Client
                    const bool registered = WaitUntil(
                        [&control, &session]() {
                            std::lock_guard<std::mutex> lock(control->mutex);
                            session = control->session.lock();
                            return static_cast<bool>(session);
                        },
                        options.timeout);
                    if (!registered) throw std::runtime_error("proxy relay active session missing");
                    session->RequestStop("relay_shutdown");
                    const bool stopped = WaitUntil(
                        [&counters]() {
                            return counters->upstreamClientOwnerStopEvents.load(std::memory_order_acquire) == 1;
                        },
                        options.timeout);
                    relay.Shutdown(); // owner stop 完成后停止 listener 与 ingress worker
                    const double elapsed = std::max(
                        1e-12,
                        std::chrono::duration<double>(Clock::now() - started).count());
                    const bool converged = shutdownOwned();
                    if (!stopped || !converged) throw std::runtime_error("proxy relay active shutdown timed out");
                    return SnapshotStats(*counters, options, elapsed);
                }
                if (options.scenario == ProxyRelayScenario::EarlyQueueOverflow
                    || options.scenario == ProxyRelayScenario::UpstreamWriteOverflow
                    || options.scenario == ProxyRelayScenario::ConnectFailure) {
                    const bool stopped = WaitUntil(
                        [&counters]() {
                            return counters->upstreamClientOwnerStopEvents.load(std::memory_order_acquire) == 1;
                        },
                        options.timeout);
                    const double elapsed = std::max(
                        1e-12,
                        std::chrono::duration<double>(Clock::now() - started).count());
                    const bool converged = shutdownOwned();
                    if (!stopped || !converged) throw std::runtime_error("proxy relay terminal scenario timed out");
                    return SnapshotStats(*counters, options, elapsed);
                }
                const bool completed = WaitUntil(
                    [&counters, &options]() {
                        const bool payloadComplete =
                            counters->backendReceivedBytes.load(std::memory_order_acquire)
                                    >= options.payloadBytes
                            && counters->clientReceivedBytes.load(std::memory_order_acquire)
                                >= options.payloadBytes;
                        const bool ingressWatermarkComplete =
                            options.ingressWriteHighWatermarkBytes == 0
                            || (counters->ingressHighWatermarkEvents.load(std::memory_order_acquire) > 0
                                && counters->ingressLowWatermarkEvents.load(std::memory_order_acquire) > 0);
                        const bool upstreamWatermarkComplete =
                            options.upstreamWriteHighWatermarkBytes == 0
                            || (counters->upstreamHighWatermarkEvents.load(std::memory_order_acquire) > 0
                                && counters->upstreamLowWatermarkEvents.load(std::memory_order_acquire) > 0);
                        // 对端收包早于本地 send completion 时，继续等待背压回落回调。
                        return payloadComplete && ingressWatermarkComplete && upstreamWatermarkComplete;
                    },
                    options.timeout);
                const double elapsed = std::max(
                    1e-12,
                    std::chrono::duration<double>(Clock::now() - started).count());

                loadConnection->Shutdown();
                const bool converged = shutdownOwned();
                if (!completed) {
                    counters->errors.fetch_add(1, std::memory_order_relaxed);
                    RecordCloseReason(counters, "benchmark_timeout");
                    throw std::runtime_error("proxy relay benchmark timed out");
                }
                if (!converged) {
                    std::ostringstream diagnostic; // 失败证据必须指出未收敛的具体 owner。
                    diagnostic << "proxy relay resources did not converge"
                        << " load_stopped=" << (load.GetStatus() == Client::Status::Stopped ? 1 : 0)
                        << " relay_stopped=" << (relay.GetStatus() == Server::Status::Stopped ? 1 : 0)
                        << " backend_stopped=" << (backend.GetStatus() == Server::Status::Stopped ? 1 : 0)
                        << " worker_exited=" << (counters->delayWorkerExited.load() ? 1 : 0)
                        << " owner_stop_events=" << counters->upstreamClientOwnerStopEvents.load()
                        << " wrong_thread_stops=" << counters->upstreamClientWrongThreadStops.load();
                    throw std::runtime_error(diagnostic.str());
                }
                return SnapshotStats(*counters, options, elapsed);
            }
        }
    }
}
