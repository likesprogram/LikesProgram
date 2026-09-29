#include <LikesProgram/Net/Connection.hpp>
#include <LikesProgram/Net/BufferChain.hpp>
#include <LikesProgram/Net/DtlsEngineFactory.hpp>
#include <LikesProgram/Net/EventLoop.hpp>
#include <LikesProgram/Net/TlsEngine.hpp>
#include <LikesProgram/Net/TlsEngineFactory.hpp>
#include "net/DatagramCompletionPolicy.hpp"
#include "net/DtlsSessionManager.hpp"
#include "net/PollerAccess.hpp"
#include "net/Transport.hpp"
#include "net/WriteQueuePolicy.hpp"
#include "net/platform/SocketOps.hpp"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace LikesProgram {
    namespace Net {
        struct Connection::ConnectionImpl {
            SocketType m_fd = kInvalidSocket;                  // 当前连接 socket
            EventLoop* m_loop = nullptr;                       // 所属事件循环，不拥有生命周期
            std::unique_ptr<Transport> m_transport;            // TCP/UDP 或用户派生安全传输对象
            std::unique_ptr<Channel> m_channel;                // 连接专属 Channel
            std::thread::id m_startingThreadId{};              // 当前 Start 回调实际执行线程
            std::atomic<bool> m_startingCallbacks{ false };     // release/acquire 发布 startingThreadId
            std::atomic<bool> m_started{ false };               // Start 后拒绝修改数据报接收容量
            TransportKind m_transportKind = TransportKind::Tcp; // 当前 socket 的 stream/datagram 语义
            Address m_remoteAddress;                           // 对端地址缓存
            Address m_localAddress;                            // 本端地址缓存
            Address m_lastDatagramPeer;                        // 最近一次 issuer 收到的数据报 peer
            std::mutex m_datagramPeerMutex;                    // 保护跨线程默认 UDP 回复 peer 快照
            std::size_t m_maxDatagramBytes = 64 * 1024;        // 单次 recvmsg 的固定容量上限
            DtlsEngineFactory m_dtlsFactory;                   // UDP DTLS 固定角色与 Engine 创建入口
            std::size_t m_dtlsMaximumCiphertextDatagramBytes = 1200; // Engine 输出 UDP payload 上限
            std::chrono::milliseconds m_dtlsHandshakeTimeout{ 30000 }; // 固定握手总期限
            std::chrono::milliseconds m_dtlsIdleTimeout{ 300000 }; // Active 会话空闲期限
            Internal::DtlsSessionLimits m_dtlsSessionLimits{ 1024, 256, 256 * 1024 }; // 每连接资源上限
            std::unique_ptr<Internal::DtlsSessionManager> m_dtlsSessionManager; // issuer-only DTLS 会话表
            Buffer m_inBuffer;                                 // 输入缓冲区
            Buffer m_outBuffer;                                // 输出缓冲区
            CloseCallback m_closeCallback;                     // 框架关闭回调
            Internal::WriteQueuePolicy m_writeQueuePolicy;     // completion/readiness 共用的写队列反馈状态
            std::mutex m_applicationSendMutex;                 // 原子化跨线程 Send 接受与 Shutdown 写闸门
            std::atomic<std::size_t> m_queuedApplicationSends{ 0 }; // 已接受但尚未进入 completion 写链的任务数
            std::atomic<bool> m_readingPaused{ false };         // 业务读暂停标记，握手读不受影响
            std::atomic<State> m_state{ State::Connected };    // 连接状态机
            std::atomic<bool> m_gracefulShutdownRequested{ false }; // 仅允许 Shutdown 前已接受任务跨越 Closing
            bool m_directPollerIo = false;                     // built-in plain TCP 是否由 Poller 直接异步接管
            bool m_completionOwnedSocket = false;              // built-in socket 不经过公开 virtual Transport
            TlsEngineFactory m_tlsFactory;                     // 用户配置的 Engine 创建与共享资源状态
            std::unique_ptr<TlsEngine> m_tlsEngine;             // 当前连接独占的 TLS 会话
            std::atomic<std::int64_t> m_tlsHandshakeTimeoutMs{ 10000 }; // TLS 握手超时毫秒，0 表示禁用
            Poller::TimeoutId m_tlsHandshakeTimeoutId = Poller::InvalidTimeoutId; // 当前握手 timer completion
            bool m_tlsHandshakeNotified = false;                // OnHandshakeDone 单次通知闸门
            bool m_tlsCloseAfterWrite = false;                  // close_notify 密文排空后关闭 socket
            bool m_shutdownWriteStarted = false;                // issuer 线程只启动一次安全层/写端关闭

            ConnectionImpl()
                : m_state(State::Connected) {
            }
        };

        namespace {
            const Address& EmptyAddress() {
                static const Address empty; // moved-from/closed 连接的稳定空地址引用
                return empty;
            }
        }

        Connection::Connection(SocketType fd, EventLoop* loop)
            : m_impl(new ConnectionImpl{}) {
            m_impl->m_fd = fd;
            m_impl->m_loop = loop;
            m_impl->m_completionOwnedSocket = true;
            m_impl->m_remoteAddress = Address::GetRemoteAddress(fd);
            m_impl->m_localAddress = Address::GetLocalAddress(fd);
        }

        Connection::Connection(SocketType fd, EventLoop* loop, TransportKind kind)
            : Connection(fd, loop) {
            if (m_impl) m_impl->m_transportKind = kind;
        }

        Connection::~Connection() {
            DoClose(false);
            delete m_impl;
            m_impl = nullptr;
        }

        void Connection::Start() {
            if (m_impl == nullptr || m_impl->m_loop == nullptr || m_impl->m_fd == kInvalidSocket) return;
            auto& startingCallbacks = m_impl->m_startingCallbacks;
            m_impl->m_started.store(true, std::memory_order_release);

            if (m_impl->m_transport) {
                const IoResult initResult = m_impl->m_transport->InitializeSecureLayer(); // 允许用户派生类创建 TLS/SSL 会话
                if (initResult.status == IoStatus::Error) {
                    OnError(initResult.error);
                    DoClose(true);
                    return;
                }
                if (initResult.status == IoStatus::PeerClosed) {
                    DoClose(true);
                    return;
                }
            }

            const auto startDtlsIfConfigured = [this]() {
                if (!m_impl->m_dtlsFactory) return true;
                if (m_impl->m_transportKind != TransportKind::Udp || !m_impl->m_directPollerIo) {
                    try {
                        OnError(EINVAL);
                    }
                    catch (...) {
                        // 无效配置仍必须由调用方继续统一关闭。
                    }
                    return false;
                }
                if (!m_impl->m_dtlsSessionManager) {
                    m_impl->m_dtlsSessionManager =
                        std::make_unique<Internal::DtlsSessionManager>(this, m_impl->m_loop);
                    m_impl->m_dtlsSessionManager->Configure(
                        m_impl->m_dtlsFactory,
                        m_impl->m_dtlsMaximumCiphertextDatagramBytes,
                        m_impl->m_dtlsHandshakeTimeout,
                        m_impl->m_dtlsIdleTimeout,
                        m_impl->m_dtlsSessionLimits);
                }
                return m_impl->m_dtlsFactory.Role() != DtlsRole::Client
                    || m_impl->m_dtlsSessionManager->StartConnectedClient();
            };

            if (TryStartPollerIo()) {
                // Poller 操作会在本轮任务末尾批量提交，先完成连接建立回调。
                m_impl->m_startingThreadId = std::this_thread::get_id();
                startingCallbacks.store(true, std::memory_order_release);
                bool dtlsStarted = true; // Client 首个 flight 是否已被 Poller 接受
                try {
                    OnSecureLayerReady();
                    if (GetState() != State::Closed) dtlsStarted = startDtlsIfConfigured();
                    if (dtlsStarted && GetState() != State::Closed) OnConnected();
                }
                catch (...) {
                    startingCallbacks.store(false, std::memory_order_release);
                    throw;
                }
                startingCallbacks.store(false, std::memory_order_release);
                if (!dtlsStarted && GetState() != State::Closed) DoClose(true);
                return;
            }

            if (m_impl->m_completionOwnedSocket) {
                // Linux 主路径不允许在直接 completion 启动失败后退回第二套 readiness 状态机。
                try {
                    OnError(m_impl->m_loop ? m_impl->m_loop->PollerRef().LastError() : 0);
                }
                catch (...) {
                    // 启动错误观察者失败也必须继续统一关闭，不能依赖外层 factory 捕获。
                }
                DoClose(true);
                return;
            }

            m_impl->m_channel = std::make_unique<Channel>(m_impl->m_loop, m_impl->m_fd, IOEvent::None);
            m_impl->m_channel->SetReadCallback([this]() { HandleRead(); });
            m_impl->m_channel->SetWriteCallback([this]() { HandleWrite(); });
            m_impl->m_channel->SetCloseCallback([this]() { HandleClose(); });
            m_impl->m_channel->SetErrorCallback([this]() { HandleError(); });

            (void)m_impl->m_loop->RegisterChannel(m_impl->m_channel.get());

            m_impl->m_startingThreadId = std::this_thread::get_id();
            startingCallbacks.store(true, std::memory_order_release);
            bool dtlsStarted = true; // readiness 兼容路径仍保持同一回调顺序
            try {
                OnSecureLayerReady();
                if (GetState() != State::Closed) dtlsStarted = startDtlsIfConfigured();
                if (dtlsStarted && GetState() != State::Closed) {
                    RefreshTransportInterest();
                    OnConnected();
                }
            }
            catch (...) {
                startingCallbacks.store(false, std::memory_order_release);
                throw;
            }
            startingCallbacks.store(false, std::memory_order_release);
            if (!dtlsStarted && GetState() != State::Closed) DoClose(true);
        }

        SocketType Connection::GetSocket() const noexcept {
            return m_impl ? m_impl->m_fd : kInvalidSocket;
        }

        Connection::State Connection::GetState() const noexcept {
            return m_impl ? m_impl->m_state.load(std::memory_order_acquire) : State::Closed;
        }

        bool Connection::IsConnected() const noexcept {
            return GetState() == State::Connected;
        }

        TransportKind Connection::GetTransportKind() const noexcept {
            return m_impl ? m_impl->m_transportKind : TransportKind::Tcp;
        }

        std::size_t Connection::GetMaxDatagramBytesForPoller() const noexcept {
            return m_impl ? m_impl->m_maxDatagramBytes : 0;
        }

        void Connection::SetFrameworkCloseCallback(CloseCallback callback) {
            if (m_impl) m_impl->m_closeCallback = std::move(callback);
        }

        void Connection::SetTlsEngineFactory(const TlsEngineFactory& factory) {
            if (m_impl == nullptr || m_impl->m_tlsEngine) return;
            m_impl->m_tlsFactory = factory;
        }

        bool Connection::HasTlsEngineFactory() const noexcept {
            return m_impl != nullptr && static_cast<bool>(m_impl->m_tlsFactory);
        }

        void Connection::SetTlsHandshakeTimeout(std::chrono::milliseconds timeout) {
            if (m_impl == nullptr || m_impl->m_tlsEngine) return;

            const std::int64_t timeoutMs = std::max<std::int64_t>(0, timeout.count()); // 负值按禁用处理
            m_impl->m_tlsHandshakeTimeoutMs.store(timeoutMs, std::memory_order_release);
        }

        void Connection::SetDtlsEngineFactory(const DtlsEngineFactory& factory) {
            if (m_impl == nullptr || m_impl->m_started.load(std::memory_order_acquire)) return;
            if (m_impl->m_transportKind != TransportKind::Udp) {
                throw std::invalid_argument("DTLS Engine Factory requires UDP transport");
            }
            m_impl->m_dtlsFactory = factory;
        }

        bool Connection::HasDtlsEngineFactory() const noexcept {
            return m_impl != nullptr && static_cast<bool>(m_impl->m_dtlsFactory);
        }

        void Connection::SetDtlsMaximumCiphertextDatagramBytes(std::size_t maxBytes) {
            constexpr std::size_t kMaximumCiphertextBytes = 64 * 1024; // 与接收 Buffer 硬上限一致
            if (m_impl == nullptr || m_impl->m_started.load(std::memory_order_acquire)) return;
            if (m_impl->m_transportKind != TransportKind::Udp) {
                throw std::invalid_argument("DTLS ciphertext limit requires UDP transport");
            }
            if (maxBytes == 0 || maxBytes > kMaximumCiphertextBytes) {
                throw std::invalid_argument("DTLS ciphertext bytes must be between 1 and 65536");
            }
            m_impl->m_dtlsMaximumCiphertextDatagramBytes = maxBytes;
        }

        void Connection::SetDtlsHandshakeTimeout(std::chrono::milliseconds timeout) {
            if (m_impl == nullptr || m_impl->m_started.load(std::memory_order_acquire)) return;
            if (m_impl->m_transportKind != TransportKind::Udp || timeout.count() < 0) {
                throw std::invalid_argument("DTLS handshake timeout requires UDP and non-negative delay");
            }
            m_impl->m_dtlsHandshakeTimeout = timeout;
        }

        void Connection::SetDtlsSessionIdleTimeout(std::chrono::milliseconds timeout) {
            if (m_impl == nullptr || m_impl->m_started.load(std::memory_order_acquire)) return;
            if (m_impl->m_transportKind != TransportKind::Udp || timeout.count() < 0) {
                throw std::invalid_argument("DTLS idle timeout requires UDP and non-negative delay");
            }
            m_impl->m_dtlsIdleTimeout = timeout;
        }

        void Connection::SetDtlsSessionLimits(
            std::size_t maxSessions,
            std::size_t maxPendingSessions,
            std::size_t maxPendingCiphertextBytesPerSession) {
            if (m_impl == nullptr || m_impl->m_started.load(std::memory_order_acquire)) return;
            if (m_impl->m_transportKind != TransportKind::Udp) {
                throw std::invalid_argument("DTLS session limits require UDP transport");
            }
            if (maxSessions == 0 || maxPendingSessions == 0 || maxPendingSessions > maxSessions
                || maxPendingCiphertextBytesPerSession == 0) {
                throw std::invalid_argument("DTLS session limits must be non-zero and internally ordered");
            }
            m_impl->m_dtlsSessionLimits = {
                maxSessions,
                maxPendingSessions,
                maxPendingCiphertextBytesPerSession
            };
        }

        void Connection::CloseDtlsSession(const Address& peer) {
            if (m_impl == nullptr || !peer.IsValid()) return;
            Address peerSnapshot(peer); // 跨线程任务持有 sockaddr 值快照
            RunInLoop([this, peerSnapshot]() {
                if (m_impl && m_impl->m_dtlsSessionManager) {
                    m_impl->m_dtlsSessionManager->CloseSession(peerSnapshot);
                }
            });
        }

        DtlsSessionStats Connection::GetDtlsSessionStats() const noexcept {
            return m_impl != nullptr && m_impl->m_dtlsSessionManager
                ? m_impl->m_dtlsSessionManager->Stats()
                : DtlsSessionStats{};
        }

        void Connection::SetMaxDatagramBytes(std::size_t maxBytes) {
            constexpr std::size_t kMaximumDatagramBytes = 64 * 1024; // 覆盖 IPv4/IPv6 最大 UDP payload
            if (m_impl == nullptr || m_impl->m_started.load(std::memory_order_acquire)) return;
            if (maxBytes == 0 || maxBytes > kMaximumDatagramBytes) {
                throw std::invalid_argument("UDP max datagram bytes must be between 1 and 65536");
            }
            m_impl->m_maxDatagramBytes = maxBytes;
        }

        void Connection::Send(const Buffer& buffer) {
            if (GetState() != State::Connected) return;
            if (buffer.ReadableBytes() == 0 && GetTransportKind() != TransportKind::Udp) return;

            Buffer payload(buffer); // const 发送保留调用方内容，复制只发生在 API 所有权边界
            Send(std::move(payload));
        }

        void Connection::Send(Buffer&& buffer) {
            if (m_impl == nullptr) return;
            if (GetTransportKind() == TransportKind::Udp) {
                Address peer; // 空地址表示复用 connected socket 的默认对端
                const bool connected = m_impl->m_remoteAddress.IsValid(); // getpeername 成功即使用 send
                if (!connected) {
                    std::lock_guard<std::mutex> peerLock(m_impl->m_datagramPeerMutex);
                    peer = m_impl->m_lastDatagramPeer;
                }
                const Internal::DatagramWriteTarget target =
                    Internal::ResolveDatagramWriteTarget(connected, peer.IsValid());
                if (target != Internal::DatagramWriteTarget::Invalid) {
                    SendDatagram(peer, std::move(buffer));
                }
                return;
            }
            if (buffer.ReadableBytes() == 0) return;

            if (m_impl->m_loop != nullptr &&
                (m_impl->m_loop->IsInLoopThread() || IsInStartingCallbackThread())) {
                if (GetState() != State::Connected) return;
                // 回调栈内直接把 Buffer 所有权交给 Poller，不复制 payload 字节。
                SendInLoop(std::move(buffer));
                return;
            }

            auto payload = std::make_shared<Buffer>(std::move(buffer)); // std::function 任务共享控制块，payload 只移动
            std::lock_guard<std::mutex> lock(m_impl->m_applicationSendMutex); // 与 Shutdown 的 Closing 发布串行
            if (GetState() != State::Connected || m_impl->m_loop == nullptr) return;

            m_impl->m_queuedApplicationSends.fetch_add(1, std::memory_order_acq_rel);
            try {
                m_impl->m_loop->PostTask([this, payload]() {
                    try {
                        SendInLoop(std::move(*payload), true);
                    }
                    catch (...) {
                        FinishQueuedSend();
                        throw;
                    }
                    FinishQueuedSend();
                });
            }
            catch (...) {
                m_impl->m_queuedApplicationSends.fetch_sub(1, std::memory_order_acq_rel);
                throw;
            }
        }

        void Connection::Send(const void* data, std::size_t len) {
            if (GetState() != State::Connected || (data == nullptr && len != 0)) return;
            if (len == 0 && GetTransportKind() != TransportKind::Udp) return;

            Buffer payload(0); // raw pointer 生命周期未知，必须在 API 边界复制一次进入自有 Buffer
            if (len != 0) payload.Append(data, len);
            Send(std::move(payload));
        }

        void Connection::SendTo(const Address& peer, Buffer&& buffer) {
            if (m_impl == nullptr || GetTransportKind() != TransportKind::Udp || !peer.IsValid()) return;
            SendDatagram(peer, std::move(buffer));
        }

        void Connection::SendDatagram(const Address& peer, Buffer&& buffer) {
            if (m_impl == nullptr || GetTransportKind() != TransportKind::Udp) return;
            const bool connected = m_impl->m_remoteAddress.IsValid(); // 空 peer 只允许 connected socket
            if (Internal::ResolveDatagramWriteTarget(connected, peer.IsValid())
                == Internal::DatagramWriteTarget::Invalid) return;

            if (m_impl->m_loop != nullptr
                && (m_impl->m_loop->IsInLoopThread() || IsInStartingCallbackThread())) {
                if (GetState() == State::Connected) SendDatagramInLoop(peer, std::move(buffer));
                return;
            }

            auto payload = std::make_shared<Buffer>(std::move(buffer)); // 空包也保留一次显式发送任务
            Address peerSnapshot(peer); // sockaddr 生命周期覆盖跨线程 PostTask
            std::lock_guard<std::mutex> lock(m_impl->m_applicationSendMutex);
            if (GetState() != State::Connected || m_impl->m_loop == nullptr) return;

            m_impl->m_queuedApplicationSends.fetch_add(1, std::memory_order_acq_rel);
            try {
                m_impl->m_loop->PostTask([this, peerSnapshot, payload]() mutable {
                    try {
                        SendDatagramInLoop(peerSnapshot, std::move(*payload), true);
                    }
                    catch (...) {
                        FinishQueuedSend();
                        throw;
                    }
                    FinishQueuedSend();
                });
            }
            catch (...) {
                m_impl->m_queuedApplicationSends.fetch_sub(1, std::memory_order_acq_rel);
                throw;
            }
        }

        void Connection::SendTo(const Address& peer, const void* data, std::size_t len) {
            if ((data == nullptr && len != 0) || GetState() != State::Connected) return;

            Buffer payload(0); // 数据报在 API 边界复制后由 Poller 独占到 CQE
            if (len != 0) payload.Append(data, len);
            SendTo(peer, std::move(payload));
        }

        void Connection::SetWriteWatermark(std::size_t highWatermarkBytes, std::size_t lowWatermarkBytes) {
            if (m_impl == nullptr) return;
            m_impl->m_writeQueuePolicy.ConfigureWatermark(
                highWatermarkBytes,
                lowWatermarkBytes); // 配置线程只发布原子阈值，不触碰 Poller 队列
        }

        void Connection::SetMaxPendingWriteBytes(std::size_t maxBytes) {
            if (m_impl) m_impl->m_writeQueuePolicy.ConfigureLimit(maxBytes);
        }

        void Connection::PauseReading() {
            if (m_impl == nullptr) return;

            m_impl->m_readingPaused.store(true, std::memory_order_release);
            RunInLoop([this]() {
                if (m_impl == nullptr) return;
                if (m_impl->m_directPollerIo && m_impl->m_loop != nullptr) {
                    m_impl->m_loop->SetPollerReadEnabled(this, false);
                }
                else if (m_impl->m_channel != nullptr) {
                    m_impl->m_channel->DisableReading();
                }
            });
        }

        void Connection::ResumeReading() {
            if (m_impl == nullptr) return;

            m_impl->m_readingPaused.store(false, std::memory_order_release);
            RunInLoop([this]() {
                if (m_impl == nullptr) return;
                if (m_impl->m_directPollerIo && m_impl->m_loop != nullptr) {
                    m_impl->m_loop->SetPollerReadEnabled(this, true);
                }
                else {
                    RefreshTransportInterest();
                }
            });
        }

        void Connection::RunInLoop(Task task) {
            if (!task) return;
            if (m_impl != nullptr && m_impl->m_loop != nullptr && m_impl->m_loop->IsInLoopThread()) {
                task();
            }
            else if (IsInStartingCallbackThread()) {
                task();
            }
            else {
                QueueInLoop(std::move(task));
            }
        }

        void Connection::QueueInLoop(Task task) {
            if (!task) return;
            if (m_impl == nullptr || m_impl->m_loop == nullptr) return;
            m_impl->m_loop->PostTask(std::move(task));
        }

        void Connection::Shutdown() {
            if (m_impl == nullptr) return;
            {
                std::lock_guard<std::mutex> lock(m_impl->m_applicationSendMutex); // Send 要么先登记任务，要么看到 Closing
                State expected = State::Connected; // API 返回前关闭新应用写和连接池复用资格
                if (!m_impl->m_state.compare_exchange_strong(
                    expected,
                    State::Closing,
                    std::memory_order_acq_rel)) return;
                m_impl->m_gracefulShutdownRequested.store(true, std::memory_order_release);
            }

            RunInLoop([this]() { ShutdownInLoop(); });
        }

        void Connection::ForceClose() {
            QueueInLoop([this]() { DoClose(true); });
        }

        void Connection::UpgradeCommunication() {
            RunInLoop([this]() {
                if (m_impl == nullptr || GetState() == State::Closed || m_impl->m_tlsEngine) return;

                if (m_impl->m_tlsFactory) {
                    try {
                        if (!m_impl->m_directPollerIo
                            || !m_impl->m_tlsFactory.InitializeSharedResources()) {
                            OnError(EIO);
                            DoClose(true);
                            return;
                        }

                        m_impl->m_tlsEngine = m_impl->m_tlsFactory.Create();
                        if (!m_impl->m_tlsEngine) {
                            OnError(ENOMEM);
                            DoClose(true);
                            return;
                        }

                        BufferChain plaintextOutput; // StartHandshake 不产生应用明文
                        BufferChain ciphertextOutput; // client/server hello 等首批握手记录
                        const TlsResult result = m_impl->m_tlsEngine->StartHandshake(ciphertextOutput);
                        if (HandleTlsResult(result, plaintextOutput, ciphertextOutput)) {
                            ArmTlsHandshakeTimeout();
                        }
                    }
                    catch (...) {
                        // Engine 初始化或首轮握手异常必须关闭 completion 连接。
                        DoClose(true);
                    }
                    return;
                }

                if (!m_impl->m_transport) return;

                const IoResult result = m_impl->m_transport->UpgradeCommunication(); // 由派生 transport 决定是否进入握手态
                if (result.status == IoStatus::Ok || result.status == IoStatus::WouldBlock) {
                    RefreshTransportInterest();
                    EnableWritingIfNeeded();
                    return;
                }

                if (result.status == IoStatus::PeerClosed) {
                    DoClose(true);
                    return;
                }

                OnError(result.error);
                DoClose(true);
            });
        }

        void Connection::HandleRead() {
            if (m_impl == nullptr || !m_impl->m_transport || GetState() == State::Closed) return;

            if (m_impl->m_transport->NeedHandshake()) {
                (void)AdvanceHandshake();
                return;
            }

            if (m_impl->m_readingPaused.load(std::memory_order_acquire)) return;

            const IoResult result = m_impl->m_transport->ReadSome(m_impl->m_inBuffer);
            if (result.status == IoStatus::Ok) {
                OnMessage(m_impl->m_inBuffer);
            }
            else if (result.status == IoStatus::PeerClosed) {
                HandleClose();
            }
            else if (result.status == IoStatus::Error) {
                OnError(result.error);
                HandleClose();
            }
        }

        void Connection::HandleWrite() {
            if (m_impl == nullptr || !m_impl->m_transport || !m_impl->m_channel || GetState() == State::Closed) return;

            if (m_impl->m_transport->NeedHandshake()) {
                if (!AdvanceHandshake() || m_impl->m_transport->NeedHandshake()) return;
            }

            while (m_impl->m_outBuffer.ReadableBytes() > 0) {
                const IoResult result = m_impl->m_transport->WriteSome(
                    m_impl->m_outBuffer.Peek(),
                    m_impl->m_outBuffer.ReadableBytes());
                if (result.status == IoStatus::Ok) {
                    m_impl->m_outBuffer.Consume(static_cast<std::size_t>(std::max<std::int64_t>(result.nbytes, 0)));
                    ApplyWriteQueueDrainBackpressure();
                    if (result.nbytes == 0) break;
                }
                else if (result.status == IoStatus::WouldBlock) {
                    break;
                }
                else {
                    OnError(result.error);
                    DoClose(true);
                    return;
                }
            }

            if (m_impl->m_outBuffer.ReadableBytes() == 0) {
                m_impl->m_channel->DisableWriting();
                OnWriteComplete();
                if (GetState() == State::Closing && m_impl->m_transport) m_impl->m_transport->ShutdownWrite();
            }
        }

        void Connection::HandleClose() {
            DoClose(true);
        }

        void Connection::HandleError() {
            OnError(0);
        }

        const Address& Connection::GetRemoteAddress() const noexcept {
            return m_impl ? m_impl->m_remoteAddress : EmptyAddress();
        }

        const Address& Connection::GetLocalAddress() const noexcept {
            return m_impl ? m_impl->m_localAddress : EmptyAddress();
        }

        bool Connection::TryStartPollerIo() {
            if (m_impl == nullptr || m_impl->m_loop == nullptr) return false;

            if (m_impl->m_completionOwnedSocket) {
                m_impl->m_directPollerIo = m_impl->m_loop->StartPollerIo(this);
                return m_impl->m_directPollerIo;
            }
            // completion-owned socket 启动失败时不回退第二套 readiness 状态机。
            return false;
        }

        bool Connection::IsInStartingCallbackThread() const noexcept {
            return m_impl != nullptr
                && m_impl->m_startingCallbacks.load(std::memory_order_acquire)
                && m_impl->m_startingThreadId == std::this_thread::get_id();
        }

        void Connection::HandlePollerRead(BufferLease&& lease) {
            if (m_impl == nullptr || lease.Empty() || GetState() == State::Closed) return;

            try {
                Buffer input(0); // lease 物化与统一处理均收敛在 Connection 异常边界内
                input.Append(std::move(lease));
                HandlePollerInput(std::move(input));
            }
            catch (...) {
                DoClose(true); // 分配失败时保持 lease RAII 归还与统一关闭语义
            }
        }

        void Connection::HandlePollerRead(Buffer&& input) {
            HandlePollerInput(std::move(input));
        }

        void Connection::HandlePollerInput(Buffer&& input) {
            if (m_impl == nullptr || input.ReadableBytes() == 0 || GetState() == State::Closed) return;

            try {
                if (m_impl->m_tlsEngine) {
                    BufferChain ciphertextInput; // 直接接管 lease 或 owning Buffer 的 socket completion
                    BufferChain plaintextOutput; // Engine 产生的业务明文链
                    BufferChain ciphertextOutput; // 握手推进或 key update 产生的响应密文链
                    ciphertextInput.Append(std::move(input));
                    const TlsResult result = m_impl->m_tlsEngine->ConsumeCiphertext(
                        ciphertextInput,
                        plaintextOutput,
                        ciphertextOutput);
                    if (!ciphertextInput.Empty()) {
                        // Engine 不得在返回后遗失未消费 completion 数据。
                        HandlePollerError(EIO);
                        return;
                    }
                    (void)HandleTlsResult(result, plaintextOutput, ciphertextOutput);
                    return;
                }

                if (m_impl->m_inBuffer.ReadableBytes() == 0) {
                    m_impl->m_inBuffer = std::move(input); // 空目标直接接管 owning completion 存储
                }
                else {
                    const std::size_t readableBytes = input.ReadableBytes(); // 仅追加来源当前可读区域
                    m_impl->m_inBuffer.Append(input.Peek(), readableBytes);
                    input.Consume(readableBytes); // 追加成功后立即释放来源拥有权
                }
                OnMessage(m_impl->m_inBuffer);
            }
            catch (...) {
                // Engine 或业务回调异常必须在 Connection 内收敛，不能击穿 Poller CQE 分派。
                DoClose(true);
            }
        }

        void Connection::HandlePollerDatagram(
            Buffer& input,
            const Address& peer,
            std::size_t originalBytes,
            bool truncated) {
            if (m_impl == nullptr || GetState() == State::Closed) return;
            {
                std::lock_guard<std::mutex> peerLock(m_impl->m_datagramPeerMutex);
                m_impl->m_lastDatagramPeer = peer; // 默认 Send 在回调内捕获本次 peer
            }
            try {
                if (m_impl->m_dtlsSessionManager) {
                    m_impl->m_dtlsSessionManager->ConsumeCiphertext(input, peer, truncated);
                }
                else {
                    OnDatagram(input, peer, originalBytes, truncated);
                }
            }
            catch (...) {
                DoClose(true);
            }
        }

        void Connection::HandlePollerDatagramWriteCompleted(
            const Address& peer,
            std::size_t queueCost) noexcept {
            if (m_impl != nullptr && m_impl->m_dtlsSessionManager) {
                m_impl->m_dtlsSessionManager->DatagramWriteCompleted(peer, queueCost);
            }
        }

        void Connection::HandlePollerPeerClosed() {
            if (m_impl != nullptr && GetState() != State::Closed) DoClose(true);
        }

        bool Connection::PollerReadEnabled() const noexcept {
            return m_impl != nullptr
                && GetState() != State::Closed
                && !m_impl->m_readingPaused.load(std::memory_order_acquire);
        }

        bool Connection::HandlePollerWriteGrowth(std::size_t pendingBytes) {
            return ApplyWriteQueueGrowthBackpressure(pendingBytes);
        }

        void Connection::HandlePollerWriteDrain(std::size_t pendingBytes) {
            ApplyWriteQueueDrainBackpressure(pendingBytes);
        }

        void Connection::HandlePollerWriteComplete() {
            if (m_impl == nullptr || GetState() == State::Closed) return;

            try {
                OnWriteComplete();
            }
            catch (...) {
                // 写完成业务回调失败时统一取消连接 operation，避免异常越过 CQE 边界。
                DoClose(true);
                return;
            }
            if (m_impl->m_tlsCloseAfterWrite) {
                DoClose(true);
                return;
            }
            if (GetState() == State::Closing) {
                if (m_impl->m_transport) m_impl->m_transport->ShutdownWrite();
                else if (m_impl->m_completionOwnedSocket
                    && m_impl->m_transportKind == TransportKind::Tcp) {
                    (void)Internal::ShutdownWrite(m_impl->m_fd);
                }
                else if (m_impl->m_completionOwnedSocket) {
                    DoClose(true); // UDP 写队列排空后直接关闭，不执行 stream half-close
                }
            }
        }

        void Connection::HandlePollerError(int error) {
            if (m_impl == nullptr || GetState() == State::Closed) return;

            try {
                OnError(error);
            }
            catch (...) {
                // 错误通知本身失败也必须继续关闭 socket 与回收 operation。
            }
            DoClose(true);
        }

        void Connection::HandlePollerDetached() noexcept {
            if (m_impl == nullptr) return;
            m_impl->m_directPollerIo = false;
        }

        bool Connection::HandleTlsResult(
            const TlsResult& result,
            BufferChain& plaintextOutput,
            BufferChain& ciphertextOutput) {
            if (m_impl == nullptr || GetState() == State::Closed) return false;
            if (!result.Succeeded()) {
                OnError(result.error);
                DoClose(true);
                return false;
            }

            if (!ciphertextOutput.Empty()) {
                if (!m_impl->m_directPollerIo
                    || m_impl->m_loop == nullptr
                    || !m_impl->m_loop->QueuePollerWrite(this, std::move(ciphertextOutput))) {
                    OnError(EIO);
                    DoClose(true);
                    return false;
                }
            }

            if (result.HasAction(TlsAction::PlaintextReady) && !plaintextOutput.Empty()) {
                OnMessageChain(plaintextOutput);
            }
            NotifyTlsHandshakeIfReady();

            if (result.HasAction(TlsAction::CloseTransport)) {
                m_impl->m_tlsCloseAfterWrite = true;
                m_impl->m_state.store(State::Closing, std::memory_order_release);
                const std::size_t pendingBytes = m_impl->m_loop != nullptr
                    ? m_impl->m_loop->PollerPendingWriteBytes(this)
                    : 0; // close_notify 未产生密文时立即关闭
                if (pendingBytes == 0) DoClose(true);
            }
            return GetState() != State::Closed;
        }

        void Connection::NotifyTlsHandshakeIfReady() {
            if (m_impl == nullptr
                || !m_impl->m_tlsEngine
                || m_impl->m_tlsHandshakeNotified
                || m_impl->m_tlsEngine->State() != TlsState::Active) {
                return;
            }

            CancelTlsHandshakeTimeout();
            m_impl->m_tlsHandshakeNotified = true;
            OnHandshakeDone();
        }

        void Connection::ArmTlsHandshakeTimeout() {
            if (m_impl == nullptr
                || m_impl->m_loop == nullptr
                || !m_impl->m_tlsEngine
                || m_impl->m_tlsEngine->State() != TlsState::Handshaking) {
                return;
            }

            CancelTlsHandshakeTimeout();
            const std::int64_t timeoutMs = m_impl->m_tlsHandshakeTimeoutMs.load(std::memory_order_acquire); // 配置快照
            if (timeoutMs <= 0) return;

            try {
                m_impl->m_tlsHandshakeTimeoutId = m_impl->m_loop->SchedulePollerTimeout(
                    std::chrono::milliseconds(timeoutMs),
                    [this]() { HandleTlsHandshakeTimeout(); });
            }
            catch (...) {
                // std::function 构造或后端登记失败统一收敛为 timer 提交失败。
                m_impl->m_tlsHandshakeTimeoutId = Poller::InvalidTimeoutId;
            }
            if (m_impl->m_tlsHandshakeTimeoutId == Poller::InvalidTimeoutId) {
                // 已配置超时却无法提交 timer 时失败关闭，避免留下无界握手。
                try {
                    OnError(EIO);
                }
                catch (...) {
                    // 错误通知失败不能阻断无 timer 握手的强制关闭。
                }
                DoClose(true);
            }
        }

        void Connection::CancelTlsHandshakeTimeout() noexcept {
            if (m_impl == nullptr || m_impl->m_tlsHandshakeTimeoutId == Poller::InvalidTimeoutId) return;

            const Poller::TimeoutId timeoutId = m_impl->m_tlsHandshakeTimeoutId; // 先清空，防止取消回调重入
            m_impl->m_tlsHandshakeTimeoutId = Poller::InvalidTimeoutId;
            if (m_impl->m_loop != nullptr) m_impl->m_loop->CancelPollerTimeout(timeoutId);
        }

        void Connection::HandleTlsHandshakeTimeout() {
            if (m_impl == nullptr) return;

            m_impl->m_tlsHandshakeTimeoutId = Poller::InvalidTimeoutId;
            if (!m_impl->m_tlsEngine
                || m_impl->m_tlsEngine->State() != TlsState::Handshaking
                || GetState() == State::Closed) {
                return;
            }

            try {
                OnError(ETIMEDOUT);
            }
            catch (...) {
                // timer completion 必须继续关闭连接，不能只让 Poller 吞掉用户异常。
            }
            DoClose(true);
        }

        void Connection::SendInLoop(Buffer&& buffer, bool acceptedBeforeShutdown) {
            if (m_impl == nullptr || buffer.ReadableBytes() == 0) return;

            const State state = GetState(); // graceful Shutdown 前登记的跨线程任务允许进入现有写链
            const bool canSend = state == State::Connected
                || (acceptedBeforeShutdown
                    && state == State::Closing
                    && m_impl->m_gracefulShutdownRequested.load(std::memory_order_acquire));
            if (!canSend) return;

            if (m_impl->m_tlsEngine) {
                try {
                    BufferChain plaintextInput; // 业务 Buffer 所有权直接进入 Engine
                    BufferChain plaintextOutput; // 加密路径不产生回环明文
                    BufferChain ciphertextOutput; // Engine 产生的待发送 TLS records
                    plaintextInput.Append(std::move(buffer));
                    const TlsResult result = m_impl->m_tlsEngine->ConsumePlaintext(
                        plaintextInput,
                        ciphertextOutput);
                    if (!plaintextInput.Empty()) {
                        HandlePollerError(EIO);
                        return;
                    }
                    (void)HandleTlsResult(result, plaintextOutput, ciphertextOutput);
                }
                catch (...) {
                    // 用户 Engine 加密失败不能让连接保持假 Connected 状态。
                    DoClose(true);
                }
                return;
            }

            if (m_impl->m_directPollerIo && m_impl->m_loop != nullptr
                && m_impl->m_loop->QueuePollerWrite(this, std::move(buffer))) {
                return;
            }
            if (m_impl->m_completionOwnedSocket) {
                // completion state 缺失时不能悄悄切换到 readiness 输出缓冲。
                try {
                    OnError(EIO);
                }
                catch (...) {
                    // 写提交错误观察者失败不能留下假 Connected 连接。
                }
                DoClose(true);
                return;
            }

            const std::uint8_t* data = buffer.Peek(); // 兼容 readiness transport 的当前连续段
            const std::size_t len = buffer.ReadableBytes(); // 本轮尚未同步写出的总字节数

            if (m_impl->m_transport && m_impl->m_transport->NeedHandshake()) {
                if (m_impl->m_outBuffer.ReadableBytes() == 0) m_impl->m_outBuffer = std::move(buffer);
                else m_impl->m_outBuffer.Append(data, len);
                if (!ApplyWriteQueueGrowthBackpressure()) return;
                RefreshTransportInterest();
                return;
            }

            if (m_impl->m_outBuffer.ReadableBytes() == 0 && m_impl->m_transport) {
                const IoResult result = m_impl->m_transport->WriteSome(data, len);
                if (result.status == IoStatus::Ok) {
                    const std::size_t written = static_cast<std::size_t>(std::max<std::int64_t>(result.nbytes, 0));
                    if (written >= len) {
                        OnWriteComplete();
                        return;
                    }

                    buffer.Consume(written);
                    m_impl->m_outBuffer = std::move(buffer);
                    if (!ApplyWriteQueueGrowthBackpressure()) return;
                }
                else if (result.status == IoStatus::WouldBlock) {
                    m_impl->m_outBuffer = std::move(buffer);
                    if (!ApplyWriteQueueGrowthBackpressure()) return;
                }
                else {
                    OnError(result.error);
                    DoClose(true);
                    return;
                }
            }
            else {
                m_impl->m_outBuffer.Append(data, len);
                if (!ApplyWriteQueueGrowthBackpressure()) return;
            }

            EnableWritingIfNeeded();
        }

        void Connection::SendDatagramInLoop(
            const Address& peer,
            Buffer&& buffer,
            bool acceptedBeforeShutdown) {
            if (m_impl == nullptr) return;
            const bool connected = m_impl->m_remoteAddress.IsValid(); // connected send 允许空 peer
            if (Internal::ResolveDatagramWriteTarget(connected, peer.IsValid())
                == Internal::DatagramWriteTarget::Invalid) return;

            const State state = GetState(); // Shutdown 前接受的数据报允许进入既有发送队列
            const bool canSend = state == State::Connected
                || (acceptedBeforeShutdown
                    && state == State::Closing
                    && m_impl->m_gracefulShutdownRequested.load(std::memory_order_acquire));
            if (!canSend) return;

            if (m_impl->m_dtlsSessionManager) {
                (void)m_impl->m_dtlsSessionManager->ConsumePlaintext(peer, std::move(buffer));
                return;
            }

            if (m_impl->m_directPollerIo && m_impl->m_loop != nullptr
                && m_impl->m_loop->QueuePollerDatagramWrite(this, peer, std::move(buffer))) {
                return;
            }
            try {
                OnError(EIO);
            }
            catch (...) {
                // 提交错误观察者失败仍继续关闭 datagram operation。
            }
            DoClose(true);
        }

        void Connection::FinishQueuedSend() {
            if (m_impl == nullptr) return;

            const std::size_t previous = m_impl->m_queuedApplicationSends.fetch_sub(
                1,
                std::memory_order_acq_rel); // 最后一个任务负责继续延迟的 graceful shutdown
            if (previous == 1 && GetState() == State::Closing) ShutdownInLoop();
        }

        void Connection::ShutdownInLoop() {
            if (m_impl == nullptr
                || GetState() != State::Closing
                || !m_impl->m_gracefulShutdownRequested.load(std::memory_order_acquire)
                || m_impl->m_queuedApplicationSends.load(std::memory_order_acquire) != 0
                || m_impl->m_shutdownWriteStarted) {
                return;
            }

            if (m_impl->m_dtlsSessionManager) {
                m_impl->m_dtlsSessionManager->Shutdown(true);
            }
            if (m_impl->m_tlsCloseAfterWrite) {
                // Engine 已在最后一条明文中要求关闭时，不再重复生成 close_notify。
                m_impl->m_shutdownWriteStarted = true;
                return;
            }
            m_impl->m_shutdownWriteStarted = true; // Engine shutdown 和 socket half-close 只能启动一次
            if (m_impl->m_tlsEngine) {
                try {
                    BufferChain plaintextOutput; // Shutdown 不产生应用明文
                    BufferChain ciphertextOutput; // close_notify 必须排在全部已接受明文之后
                    m_impl->m_tlsCloseAfterWrite = true;
                    const TlsResult result = m_impl->m_tlsEngine->Shutdown(ciphertextOutput);
                    (void)HandleTlsResult(result, plaintextOutput, ciphertextOutput);
                }
                catch (...) {
                    // close_notify 生成异常时强制关闭，不能永久停在 Closing。
                    DoClose(true);
                }
                return;
            }

            const std::size_t pendingBytes = m_impl->m_directPollerIo && m_impl->m_loop != nullptr
                ? m_impl->m_loop->PollerPendingWriteBytes(this)
                : m_impl->m_outBuffer.ReadableBytes(); // 已接受任务均已进入当前发送所有权链
            if (pendingBytes == 0) {
                if (m_impl->m_transport) m_impl->m_transport->ShutdownWrite();
                else if (m_impl->m_completionOwnedSocket
                    && m_impl->m_transportKind == TransportKind::Tcp) {
                    (void)Internal::ShutdownWrite(m_impl->m_fd);
                }
                else if (m_impl->m_completionOwnedSocket) {
                    DoClose(true);
                }
            }
        }

        bool Connection::AdvanceHandshake() {
            if (m_impl == nullptr || !m_impl->m_transport || !m_impl->m_transport->NeedHandshake()) return true;

            const IoResult result = m_impl->m_transport->Handshake();
            if (result.status == IoStatus::Ok) {
                if (!m_impl->m_transport->NeedHandshake()) {
                    OnHandshakeDone();
                    RefreshTransportInterest();
                    EnableWritingIfNeeded();
                }
                else {
                    RefreshTransportInterest();
                }
                return true;
            }

            if (result.status == IoStatus::WouldBlock) {
                RefreshTransportInterest();
                return true;
            }

            if (result.status == IoStatus::PeerClosed) {
                DoClose(true);
                return false;
            }

            OnError(result.error);
            DoClose(true);
            return false;
        }

        void Connection::RefreshTransportInterest() {
            if (m_impl == nullptr || !m_impl->m_channel) return;

            if (m_impl->m_transport && m_impl->m_transport->NeedHandshake()) {
                if (m_impl->m_transport->RemainWantRead()) {
                    m_impl->m_channel->EnableReading();
                }
                else {
                    m_impl->m_channel->DisableReading();
                }

                if (m_impl->m_transport->RemainWantWrite()) {
                    m_impl->m_channel->EnableWriting();
                }
                else if (m_impl->m_outBuffer.ReadableBytes() == 0) {
                    m_impl->m_channel->DisableWriting();
                }
                return;
            }

            if (m_impl->m_readingPaused.load(std::memory_order_acquire)) {
                m_impl->m_channel->DisableReading();
            }
            else {
                m_impl->m_channel->EnableReading();
            }
            if (m_impl->m_outBuffer.ReadableBytes() == 0) m_impl->m_channel->DisableWriting();
        }

        void Connection::EnableWritingIfNeeded() {
            if (m_impl != nullptr && m_impl->m_channel != nullptr && m_impl->m_outBuffer.ReadableBytes() > 0) {
                m_impl->m_channel->EnableWriting();
            }
        }

        bool Connection::ApplyWriteQueueGrowthBackpressure() {
            if (m_impl == nullptr) return false;

            const std::size_t pending = m_impl->m_outBuffer.ReadableBytes(); // 当前 loop 线程内的待发送字节
            return ApplyWriteQueueGrowthBackpressure(pending);
        }

        bool Connection::ApplyWriteQueueGrowthBackpressure(std::size_t pendingBytes) {
            if (m_impl == nullptr) return false;

            const Internal::WriteQueueSignal signal =
                m_impl->m_writeQueuePolicy.ObserveGrowth(pendingBytes); // 所有发送路径共享互斥反馈
            if (signal == Internal::WriteQueueSignal::Overflow) {
                try {
                    OnWriteQueueOverflow(pendingBytes);
                }
                catch (...) {
                    // Overflow 通知异常也必须进入同一硬上限关闭路径。
                }
                DoClose(true);
                return false;
            }
            if (signal == Internal::WriteQueueSignal::High) {
                try {
                    OnWriteHighWatermark(pendingBytes);
                }
                catch (...) {
                    // High 通知位于发送 completion 路径，异常时关闭以保持队列所有权确定。
                    DoClose(true);
                    return false;
                }
            }
            return GetState() != State::Closed;
        }

        void Connection::ApplyWriteQueueDrainBackpressure() {
            if (m_impl == nullptr) return;

            const std::size_t pending = m_impl->m_outBuffer.ReadableBytes(); // 写出后剩余队列长度
            ApplyWriteQueueDrainBackpressure(pending);
        }

        void Connection::ApplyWriteQueueDrainBackpressure(std::size_t pendingBytes) {
            if (m_impl == nullptr) return;

            const Internal::WriteQueueSignal signal =
                m_impl->m_writeQueuePolicy.ObserveDrain(pendingBytes); // CAS 避免并发禁用后的过期 Low
            if (signal == Internal::WriteQueueSignal::Low) {
                try {
                    OnWriteLowWatermark(pendingBytes);
                }
                catch (...) {
                    // partial write CQE 的 Low 通知异常不能击穿 Poller 分派。
                    DoClose(true);
                }
            }
        }

        void Connection::DoClose(bool notifyFramework) {
            if (m_impl == nullptr) return;

            State expected = State::Connected; // 首先尝试从连接态关闭
            if (!m_impl->m_state.compare_exchange_strong(expected, State::Closed, std::memory_order_acq_rel)) {
                expected = State::Closing;
                if (!m_impl->m_state.compare_exchange_strong(expected, State::Closed, std::memory_order_acq_rel)) return;
            }

            try {
                OnClosing();
            }
            catch (...) {
                // 生命周期通知不得阻断后续 cancel、close 和 lease 归还。
            }
            CancelTlsHandshakeTimeout();
            if (m_impl->m_dtlsSessionManager) {
                m_impl->m_dtlsSessionManager->Shutdown(false);
            }

            if (m_impl->m_directPollerIo && m_impl->m_loop != nullptr) {
                // 先停止并取消 Poller 操作，避免关闭 fd 后继续续投 recv/send。
                m_impl->m_loop->StopPollerIo(this);
                m_impl->m_directPollerIo = false;
            }

            if (notifyFramework && m_impl->m_channel && m_impl->m_loop) {
                m_impl->m_channel->DisableAll();
                (void)m_impl->m_loop->UnregisterChannel(m_impl->m_channel.get());
            }

            EventLoop* loop = m_impl->m_loop; // 关闭回调结束前需要保留 loop，用于延迟释放当前连接
            std::shared_ptr<Connection> self; // 摘除后继续持有自身，防止 fd 复用跨线程释放
            if (notifyFramework && m_impl->m_loop != nullptr && m_impl->m_fd != kInvalidSocket) {
                // 必须先摘除旧 fd 再关闭 socket；否则 accept 复用同 fd 会覆盖并释放当前对象。
                self = m_impl->m_loop->DetachConnection(m_impl->m_fd);
            }

            if (m_impl->m_transport) m_impl->m_transport->Close();
            else if (m_impl->m_completionOwnedSocket) Internal::CloseSocket(m_impl->m_fd);
            m_impl->m_tlsEngine.reset();
            m_impl->m_inBuffer.RetrieveAll(); // 已关闭连接不得继续占用 provided-buffer lease
            m_impl->m_outBuffer.RetrieveAll(); // readiness 兼容缓冲也在统一关闭边界释放

            if (notifyFramework && m_impl->m_closeCallback) {
                try {
                    m_impl->m_closeCallback(*this);
                }
                catch (...) {
                    // 框架观察者失败不能阻断用户关闭通知与 fd 状态收敛。
                }
            }
            try {
                OnClosed();
            }
            catch (...) {
                // 用户关闭通知失败后仍需发布无效 fd 并延迟释放当前对象。
            }
            m_impl->m_fd = kInvalidSocket;

            if (self && loop != nullptr) {
                // Channel 回调仍在栈上时不能立刻析构连接，延迟到本轮事件批次之后释放。
                try {
                    loop->PostTask([keepAlive = std::move(self)]() {
                        (void)keepAlive;
                    });
                }
                catch (...) {
                    // 极端分配失败时由当前 self 快照在函数返回后安全释放。
                }
            }
        }

        void Connection::OnDatagram(
            Buffer& input,
            const Address&,
            std::size_t,
            bool) {
            OnMessage(input); // 默认保持旧 Connection 子类的同步消息入口
        }

        void Connection::OnMessageChain(BufferChain& in) {
            if (m_impl == nullptr || in.Empty()) return;

            const std::size_t segmentCount = in.SegmentCount(); // 默认兼容路径复制到连续业务 Buffer
            for (std::size_t index = 0; index < segmentCount; ++index) {
                const BufferSlice segment = in.Segment(index); // 当前解密明文段借用视图
                m_impl->m_inBuffer.Append(segment.Data(), segment.Size());
            }
            in.Consume(in.ReadableBytes());
            OnMessage(m_impl->m_inBuffer);
        }

    }
}
