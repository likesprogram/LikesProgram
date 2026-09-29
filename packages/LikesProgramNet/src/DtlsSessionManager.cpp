#include "net/DtlsSessionManager.hpp"
#include <LikesProgram/Net/Connection.hpp>
#include <LikesProgram/Net/EventLoop.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace LikesProgram {
    namespace Net {
        namespace Internal {
            struct DtlsSessionManager::DtlsPeerKey {
                std::uint16_t m_family = AF_UNSPEC; // AF_INET/AF_INET6，AF_UNSPEC 表示无效
                std::uint16_t m_port = 0;           // 网络字节序端口，保持 sockaddr 精确值
                std::array<std::uint8_t, 16> m_address{}; // IPv4 使用前 4 字节，IPv6 使用全部
                std::uint32_t m_scopeId = 0;        // IPv6 link-local scope

                bool operator==(const DtlsPeerKey& other) const noexcept {
                    return m_family == other.m_family
                        && m_port == other.m_port
                        && m_address == other.m_address
                        && m_scopeId == other.m_scopeId;
                }
            };

            struct DtlsSessionManager::DtlsSessionManagerImpl {
                struct PeerKeyHash {
                    std::size_t operator()(const DtlsPeerKey& key) const noexcept {
                        std::size_t hash = key.m_family; // FNV 风格组合固定标量键
                        hash = (hash * 16777619U) ^ key.m_port;
                        for (std::uint8_t byte : key.m_address) hash = (hash * 16777619U) ^ byte;
                        return (hash * 16777619U) ^ key.m_scopeId;
                    }
                };

                struct Session {
                    DtlsPeerKey m_key;                       // canonical peer value key
                    Address m_peer;                          // 回调与显式 sendmsg peer 快照
                    Address m_local;                         // Factory 创建时本端快照
                    std::uint64_t m_generation = 0;          // key 重用时拒绝 stale callback
                    std::unique_ptr<DtlsEngine> m_engine;    // 当前 peer 独占 Engine
                    Poller::TimeoutId m_retransmitTimeoutId = Poller::InvalidTimeoutId; // Engine timer
                    Poller::TimeoutId m_handshakeTimeoutId = Poller::InvalidTimeoutId; // 固定握手期限
                    Poller::TimeoutId m_idleTimeoutId = Poller::InvalidTimeoutId; // Active 空闲期限
                    std::uint64_t m_retransmitTimerGeneration = 0; // timer 替换代际
                    std::uint64_t m_idleTimerGeneration = 0; // idle timer 替换代际
                    std::size_t m_pendingCiphertextCost = 0; // 尚未回收 CQE 的排队成本
                    DtlsState m_observedState = DtlsState::Handshaking; // 上次成功处理后的 Engine 状态
                    bool m_countedPending = true;            // pendingSessions 计数归属
                    bool m_countedActive = false;            // activeSessions 计数归属
                    bool m_handshakeNotified = false;        // Active 回调单次闸门
                    bool m_closing = false;                  // 拒绝新输入并保留 key 到 drain
                };

                Connection* m_connection = nullptr;         // issuer Connection，不拥有生命周期
                EventLoop* m_loop = nullptr;                 // issuer EventLoop，不拥有生命周期
                DtlsEngineFactory m_factory;                 // 固定 Client/Server 角色
                std::size_t m_maximumCiphertextDatagramBytes = 1200; // 精确 UDP payload 上限
                std::chrono::milliseconds m_handshakeTimeout{ 30000 }; // 固定握手期限
                std::chrono::milliseconds m_idleTimeout{ 300000 }; // Active 空闲期限
                DtlsSessionLimits m_limits{ 1024, 256, 256 * 1024 }; // 资源保护基线
                bool m_connectedSocket = false;              // true 时 ciphertext 使用 connected send
                bool m_shuttingDown = false;                 // 停止创建和接受业务明文
                std::uint64_t m_nextSessionGeneration = 1;   // 单调 session generation
                std::unordered_map<DtlsPeerKey, std::unique_ptr<Session>, PeerKeyHash> m_sessions; // peer 表
                std::atomic<std::uint64_t> m_createdSessions{ 0 }; // 已创建会话总数
                std::atomic<std::size_t> m_pendingSessions{ 0 }; // 当前 Handshaking 数量
                std::atomic<std::size_t> m_activeSessions{ 0 }; // 当前 Active 数量
                std::atomic<std::uint64_t> m_closedSessions{ 0 }; // 已清理会话总数
                std::atomic<std::uint64_t> m_handshakeTimeouts{ 0 }; // 握手期限到期次数
                std::atomic<std::uint64_t> m_retransmitTimeouts{ 0 }; // Engine timer 到期次数
                std::atomic<std::uint64_t> m_droppedNewPeers{ 0 }; // 资源上限拒绝次数
                std::atomic<std::uint64_t> m_fatalSessionErrors{ 0 }; // fatal Engine 结果次数

                static DtlsPeerKey MakeKey(const Address& peer) noexcept {
                    DtlsPeerKey key; // 无效地址保留 AF_UNSPEC
                    if (!peer.IsValid() || peer.SockAddr() == nullptr) return key;
                    key.m_family = static_cast<std::uint16_t>(peer.FamilyValue());
                    if (peer.FamilyValue() == AF_INET) {
                        const auto* address = reinterpret_cast<const sockaddr_in*>(peer.SockAddr());
                        key.m_port = address->sin_port;
                        std::memcpy(key.m_address.data(), &address->sin_addr, sizeof(address->sin_addr));
                    }
                    else if (peer.FamilyValue() == AF_INET6) {
                        const auto* address = reinterpret_cast<const sockaddr_in6*>(peer.SockAddr());
                        key.m_port = address->sin6_port;
                        key.m_scopeId = address->sin6_scope_id;
                        std::memcpy(key.m_address.data(), &address->sin6_addr, sizeof(address->sin6_addr));
                    }
                    else {
                        key.m_family = AF_UNSPEC;
                    }
                    return key;
                }

                Session* Find(const DtlsPeerKey& key, std::uint64_t generation = 0) noexcept {
                    const auto found = m_sessions.find(key); // issuer-only 哈希查找
                    if (found == m_sessions.end()) return nullptr;
                    if (generation != 0 && found->second->m_generation != generation) return nullptr;
                    return found->second.get();
                }
            };

            DtlsSessionManager::DtlsSessionManager(Connection* connection, EventLoop* loop)
                : m_impl(new DtlsSessionManagerImpl{}) {
                m_impl->m_connection = connection;
                m_impl->m_loop = loop;
                m_impl->m_connectedSocket = connection != nullptr
                    && connection->GetRemoteAddress().IsValid();
            }

            DtlsSessionManager::~DtlsSessionManager() {
                Shutdown(false);
                delete m_impl;
                m_impl = nullptr;
            }

            void DtlsSessionManager::Configure(
                const DtlsEngineFactory& factory,
                std::size_t maximumCiphertextDatagramBytes,
                std::chrono::milliseconds handshakeTimeout,
                std::chrono::milliseconds idleTimeout,
                DtlsSessionLimits limits) {
                if (m_impl == nullptr || !m_impl->m_sessions.empty()) return;
                m_impl->m_factory = factory;
                m_impl->m_maximumCiphertextDatagramBytes = maximumCiphertextDatagramBytes;
                m_impl->m_handshakeTimeout = handshakeTimeout;
                m_impl->m_idleTimeout = idleTimeout;
                m_impl->m_limits = limits;
            }

            bool DtlsSessionManager::StartConnectedClient() {
                if (m_impl == nullptr || m_impl->m_connection == nullptr || m_impl->m_loop == nullptr
                    || m_impl->m_shuttingDown || !m_impl->m_sessions.empty()) return false;
                const Address peer(m_impl->m_connection->GetRemoteAddress()); // connected 固定远端
                const DtlsPeerKey key = DtlsSessionManagerImpl::MakeKey(peer);
                const auto reportError = [this, &peer](int error) noexcept {
                    try { m_impl->m_connection->OnDtlsSessionError(peer, error); }
                    catch (...) {}
                    try { m_impl->m_connection->OnError(error); }
                    catch (...) {}
                    return false;
                };
                if (key.m_family == AF_UNSPEC || m_impl->m_factory.Role() != DtlsRole::Client) {
                    return reportError(EINVAL);
                }
                if (!CanCreateDtlsSession(m_impl->m_limits, 0, 0)) {
                    m_impl->m_droppedNewPeers.fetch_add(1, std::memory_order_relaxed);
                    return reportError(ENOBUFS);
                }
                if (!m_impl->m_factory.InitializeSharedResources()) return reportError(EIO);
                std::unique_ptr<DtlsEngine> engine = m_impl->m_factory.Create(
                    peer,
                    m_impl->m_connection->GetLocalAddress(),
                    m_impl->m_maximumCiphertextDatagramBytes);
                if (!engine) return reportError(ENOMEM);

                auto session = std::make_unique<DtlsSessionManagerImpl::Session>(); // 完整发布后才启动 timer
                session->m_key = key;
                session->m_peer = peer;
                session->m_local = m_impl->m_connection->GetLocalAddress();
                session->m_generation = m_impl->m_nextSessionGeneration++;
                session->m_engine = std::move(engine);
                const std::uint64_t generation = session->m_generation;
                m_impl->m_sessions.emplace(key, std::move(session));
                m_impl->m_createdSessions.fetch_add(1, std::memory_order_relaxed);
                m_impl->m_pendingSessions.fetch_add(1, std::memory_order_release);

                auto* created = m_impl->Find(key, generation); // 刚发布的唯一 Client session
                if (m_impl->m_handshakeTimeout.count() > 0) {
                    created->m_handshakeTimeoutId = m_impl->m_loop->SchedulePollerTimeout(
                        m_impl->m_handshakeTimeout,
                        [this, key, generation]() { HandleHandshakeTimeout(key, generation); });
                    if (created->m_handshakeTimeoutId == Poller::InvalidTimeoutId) {
                        return FailSession(key, generation, EIO, false);
                    }
                }
                try {
                    DtlsDatagramBatch plaintextOutput; // Client Start 不产生应用明文
                    DtlsDatagramBatch ciphertextOutput; // 首个 handshake flight
                    const DtlsResult result = created->m_engine->StartHandshake(ciphertextOutput);
                    return ProcessSessionResult(
                        key, generation, result, true, plaintextOutput, ciphertextOutput);
                }
                catch (...) {
                    return FailSession(key, generation, EIO, false);
                }
            }

            bool DtlsSessionManager::ConsumePlaintext(const Address& peer, Buffer&& plaintext) {
                if (m_impl == nullptr || m_impl->m_shuttingDown) return false;
                const Address& effectivePeer = peer.IsValid()
                    ? peer
                    : m_impl->m_connection->GetRemoteAddress(); // connected Send 使用空 peer
                const DtlsPeerKey key = DtlsSessionManagerImpl::MakeKey(effectivePeer);
                auto* session = m_impl->Find(key);
                if (session == nullptr || session->m_closing || !session->m_engine
                    || session->m_engine->State() != DtlsState::Active) return false;
                const std::uint64_t generation = session->m_generation;
                try {
                    DtlsDatagramBatch plaintextOutput; // 应用发送不产生入站明文
                    DtlsDatagramBatch ciphertextOutput; // 加密后的完整数据报
                    const DtlsResult result =
                        session->m_engine->ConsumePlaintext(plaintext, ciphertextOutput);
                    return ProcessSessionResult(
                        key,
                        generation,
                        result,
                        plaintext.ReadableBytes() == 0,
                        plaintextOutput,
                        ciphertextOutput);
                }
                catch (...) {
                    return FailSession(key, generation, EIO, m_impl->m_connectedSocket);
                }
            }

            void DtlsSessionManager::ConsumeCiphertext(
                Buffer& ciphertext,
                const Address& peer,
                bool truncated) {
                if (m_impl == nullptr || m_impl->m_shuttingDown) return;
                const Address& effectivePeer = peer.IsValid()
                    ? peer
                    : m_impl->m_connection->GetRemoteAddress(); // connected recv 可不返回 msg_name
                DtlsPeerKey key = DtlsSessionManagerImpl::MakeKey(effectivePeer);
                if (key.m_family == AF_UNSPEC) return;
                auto* session = m_impl->Find(key);
                if (truncated) {
                    if (session != nullptr) {
                        (void)FailSession(
                            key, session->m_generation, EMSGSIZE, m_impl->m_connectedSocket);
                    }
                    return; // unknown truncated peer 不创建 Engine
                }
                std::uint64_t generation = session ? session->m_generation : 0;
                if (session == nullptr) {
                    if (m_impl->m_factory.Role() != DtlsRole::Server
                        || !CreateServerSession(effectivePeer, key, generation)) return;
                    session = m_impl->Find(key, generation);
                }
                if (session == nullptr || session->m_closing || !session->m_engine) return;
                try {
                    DtlsDatagramBatch plaintextOutput; // 解密出的完整业务数据报
                    DtlsDatagramBatch ciphertextOutput; // cookie/handshake/key-update 响应
                    const DtlsResult result = session->m_engine->ConsumeCiphertext(
                        ciphertext,
                        plaintextOutput,
                        ciphertextOutput);
                    (void)ProcessSessionResult(
                        key,
                        generation,
                        result,
                        ciphertext.ReadableBytes() == 0,
                        plaintextOutput,
                        ciphertextOutput);
                }
                catch (...) {
                    (void)FailSession(key, generation, EIO, m_impl->m_connectedSocket);
                }
            }

            void DtlsSessionManager::CloseSession(const Address& peer) {
                if (m_impl == nullptr) return;
                const DtlsPeerKey key = DtlsSessionManagerImpl::MakeKey(peer);
                auto* session = m_impl->Find(key);
                if (session == nullptr || session->m_closing || !session->m_engine) return;
                const std::uint64_t generation = session->m_generation;
                try {
                    DtlsDatagramBatch plaintextOutput; // Shutdown 不产生业务明文
                    DtlsDatagramBatch ciphertextOutput; // alert/close flight
                    const DtlsResult result = session->m_engine->Shutdown(ciphertextOutput);
                    if (!ProcessSessionResult(
                        key, generation, result, true, plaintextOutput, ciphertextOutput)) return;
                }
                catch (...) {
                    (void)FailSession(key, generation, EIO, m_impl->m_connectedSocket);
                    return;
                }
                MarkSessionClosing(key, generation);
            }

            void DtlsSessionManager::Shutdown(bool graceful) {
                if (m_impl == nullptr) return;
                m_impl->m_shuttingDown = true;
                std::vector<std::pair<DtlsPeerKey, std::uint64_t>> sessions; // 回调可修改 map 的稳定快照
                sessions.reserve(m_impl->m_sessions.size());
                for (const auto& item : m_impl->m_sessions) {
                    sessions.emplace_back(item.first, item.second->m_generation);
                }
                if (graceful) {
                    for (const auto& item : sessions) {
                        auto* session = m_impl->Find(item.first, item.second);
                        if (session == nullptr || session->m_closing || !session->m_engine) continue;
                        try {
                            DtlsDatagramBatch plaintextOutput; // 每 peer 独立 close 结果
                            DtlsDatagramBatch ciphertextOutput;
                            const DtlsResult result = session->m_engine->Shutdown(ciphertextOutput);
                            (void)ProcessSessionResult(
                                item.first,
                                item.second,
                                result,
                                true,
                                plaintextOutput,
                                ciphertextOutput);
                        }
                        catch (...) {
                            (void)FailSession(
                                item.first, item.second, EIO, m_impl->m_connectedSocket);
                        }
                        MarkSessionClosing(item.first, item.second);
                    }
                    return;
                }

                std::vector<Address> closedPeers; // 清表后再通知，避免 observer 重入 map
                closedPeers.reserve(m_impl->m_sessions.size());
                for (const auto& item : sessions) {
                    auto* session = m_impl->Find(item.first, item.second);
                    if (session == nullptr) continue;
                    CancelSessionTimers(item.first, item.second);
                    if (session->m_countedPending) {
                        m_impl->m_pendingSessions.fetch_sub(1, std::memory_order_release);
                    }
                    if (session->m_countedActive) {
                        m_impl->m_activeSessions.fetch_sub(1, std::memory_order_release);
                    }
                    closedPeers.push_back(session->m_peer);
                }
                const std::size_t closedCount = m_impl->m_sessions.size(); // 本次强制清理数量
                m_impl->m_sessions.clear();
                m_impl->m_closedSessions.fetch_add(closedCount, std::memory_order_relaxed);
                for (const Address& closedPeer : closedPeers) {
                    try { m_impl->m_connection->OnDtlsSessionClosed(closedPeer); }
                    catch (...) {}
                }
            }

            void DtlsSessionManager::DatagramWriteCompleted(
                const Address& peer,
                std::size_t queueCost) noexcept {
                if (m_impl == nullptr) return;
                const Address& effectivePeer = peer.IsValid()
                    ? peer
                    : m_impl->m_connection->GetRemoteAddress();
                const DtlsPeerKey key = DtlsSessionManagerImpl::MakeKey(effectivePeer);
                auto* session = m_impl->Find(key);
                if (session == nullptr) return;
                const std::uint64_t generation = session->m_generation;
                session->m_pendingCiphertextCost = queueCost >= session->m_pendingCiphertextCost
                    ? 0
                    : session->m_pendingCiphertextCost - queueCost;
                FinalizeSessionIfDrained(key, generation);
            }

            DtlsSessionStats DtlsSessionManager::Stats() const noexcept {
                if (m_impl == nullptr) return {};
                DtlsSessionStats stats; // 逐字段 acquire 形成线程安全标量快照
                stats.createdSessions = m_impl->m_createdSessions.load(std::memory_order_acquire);
                stats.pendingSessions = m_impl->m_pendingSessions.load(std::memory_order_acquire);
                stats.activeSessions = m_impl->m_activeSessions.load(std::memory_order_acquire);
                stats.closedSessions = m_impl->m_closedSessions.load(std::memory_order_acquire);
                stats.handshakeTimeouts = m_impl->m_handshakeTimeouts.load(std::memory_order_acquire);
                stats.retransmitTimeouts = m_impl->m_retransmitTimeouts.load(std::memory_order_acquire);
                stats.droppedNewPeers = m_impl->m_droppedNewPeers.load(std::memory_order_acquire);
                stats.fatalSessionErrors = m_impl->m_fatalSessionErrors.load(std::memory_order_acquire);
                return stats;
            }

            bool DtlsSessionManager::ProcessSessionResult(
                const DtlsPeerKey& key,
                std::uint64_t sessionGeneration,
                const DtlsResult& result,
                bool inputConsumed,
                DtlsDatagramBatch& plaintextOutput,
                DtlsDatagramBatch& ciphertextOutput) {
                auto* session = m_impl ? m_impl->Find(key, sessionGeneration) : nullptr;
                if (session == nullptr || !session->m_engine) return false;
                const std::size_t ciphertextCount = ciphertextOutput.Count(); // 移动前的输出摘要
                const std::size_t plaintextCount = plaintextOutput.Count(); // 零长度明文也按元素计数
                const DtlsState engineState = session->m_engine->State(); // 本轮返回后的 Engine 状态
                const bool stateProgress = engineState != session->m_observedState; // 状态是否真实推进
                std::size_t maximumCiphertextBytes = 0; // 本轮最大 ciphertext 元素
                for (std::size_t index = 0; index < ciphertextCount; ++index) {
                    maximumCiphertextBytes = (std::max)(
                        maximumCiphertextBytes,
                        ciphertextOutput.At(index).ReadableBytes());
                }
                const DtlsResultValidation validation = ValidateDtlsResult(
                    result,
                    engineState,
                    inputConsumed,
                    ciphertextCount,
                    maximumCiphertextBytes,
                    plaintextCount,
                    m_impl->m_maximumCiphertextDatagramBytes);
                if (validation == DtlsResultValidation::ProtocolError) {
                    return FailSession(key, sessionGeneration, EPROTO, m_impl->m_connectedSocket);
                }
                if (validation == DtlsResultValidation::MessageTooLarge) {
                    return FailSession(key, sessionGeneration, EMSGSIZE, m_impl->m_connectedSocket);
                }
                if (!result.Succeeded()) {
                    m_impl->m_fatalSessionErrors.fetch_add(1, std::memory_order_relaxed);
                    return FailSession(
                        key, sessionGeneration, result.error, m_impl->m_connectedSocket);
                }
                session->m_observedState = engineState;

                Buffer ciphertext(0); // 逐项移动，保持 Engine 输出边界和顺序
                while (ciphertextOutput.TakeFront(ciphertext)) {
                    if (!QueueSessionCiphertext(
                        key, sessionGeneration, std::move(ciphertext))) return false;
                }
                if (!NotifyHandshakeIfReady(key, sessionGeneration)) return false;

                Buffer plaintext(0); // 每个元素分别触发一次 peer-aware 业务回调
                while (plaintextOutput.TakeFront(plaintext)) {
                    session = m_impl->Find(key, sessionGeneration);
                    if (session == nullptr || session->m_closing) return false;
                    const std::size_t bytes = plaintext.ReadableBytes();
                    const Address callbackPeer(session->m_peer); // observer 重入时保持 peer 生命周期
                    try {
                        m_impl->m_connection->OnDatagram(
                            plaintext, callbackPeer, bytes, false);
                    }
                    catch (...) {
                        return FailSession(
                            key, sessionGeneration, EIO, m_impl->m_connectedSocket);
                    }
                }

                session = m_impl->Find(key, sessionGeneration);
                if (session == nullptr) return false;
                if (ShouldRefreshDtlsIdle(
                    m_impl->m_idleTimeout.count() > 0,
                    engineState,
                    plaintextCount > 0,
                    ciphertextCount > 0,
                    stateProgress)
                    && !ArmIdleTimer(key, sessionGeneration)) {
                    return FailSession(key, sessionGeneration, EIO, m_impl->m_connectedSocket);
                }
                session = m_impl->Find(key, sessionGeneration);
                if (session == nullptr) return false;
                if (result.HasAction(DtlsAction::CancelRetransmitTimer)) {
                    m_impl->m_loop->CancelPollerTimeout(session->m_retransmitTimeoutId);
                    session->m_retransmitTimeoutId = Poller::InvalidTimeoutId;
                    ++session->m_retransmitTimerGeneration;
                }
                if (result.HasAction(DtlsAction::ArmRetransmitTimer)
                    && !ArmRetransmitTimer(
                        key,
                        sessionGeneration,
                        std::chrono::milliseconds(result.retransmitAfterMilliseconds))) {
                    return FailSession(key, sessionGeneration, EIO, m_impl->m_connectedSocket);
                }
                if (result.HasAction(DtlsAction::CloseSession)) {
                    MarkSessionClosing(key, sessionGeneration);
                }
                return true;
            }

            bool DtlsSessionManager::QueueSessionCiphertext(
                const DtlsPeerKey& key,
                std::uint64_t sessionGeneration,
                Buffer&& ciphertext) {
                auto* session = m_impl ? m_impl->Find(key, sessionGeneration) : nullptr;
                if (session == nullptr || m_impl->m_loop == nullptr) return false;
                const std::size_t bytes = ciphertext.ReadableBytes(); // 移动前固定 queue cost
                if (!CanQueueDtlsCiphertext(
                    m_impl->m_limits, session->m_pendingCiphertextCost, bytes)) {
                    return FailSession(
                        key, sessionGeneration, ENOBUFS, m_impl->m_connectedSocket);
                }
                Address target = m_impl->m_connectedSocket ? Address{} : session->m_peer; // send/sendmsg 目标
                if (!m_impl->m_loop->QueuePollerDatagramWrite(
                    m_impl->m_connection, target, std::move(ciphertext))) {
                    return FailSession(key, sessionGeneration, EIO, m_impl->m_connectedSocket);
                }
                session = m_impl->Find(key, sessionGeneration); // 背压回调可能同步关闭
                if (session == nullptr) return false;
                session->m_pendingCiphertextCost += DtlsDatagramQueueCost(bytes);
                return true;
            }

            bool DtlsSessionManager::NotifyHandshakeIfReady(
                const DtlsPeerKey& key,
                std::uint64_t sessionGeneration) {
                auto* session = m_impl ? m_impl->Find(key, sessionGeneration) : nullptr;
                if (session == nullptr || !session->m_engine) return false;
                if (session->m_engine->State() != DtlsState::Active
                    || session->m_handshakeNotified) return true;
                m_impl->m_loop->CancelPollerTimeout(session->m_handshakeTimeoutId);
                session->m_handshakeTimeoutId = Poller::InvalidTimeoutId;
                if (session->m_countedPending) {
                    m_impl->m_pendingSessions.fetch_sub(1, std::memory_order_release);
                    session->m_countedPending = false;
                }
                m_impl->m_activeSessions.fetch_add(1, std::memory_order_release);
                session->m_countedActive = true;
                session->m_handshakeNotified = true;
                const Address callbackPeer(session->m_peer); // 回调期间稳定 peer
                try {
                    m_impl->m_connection->OnDtlsHandshakeDone(
                        callbackPeer, session->m_engine->NegotiatedProtocol());
                    return true;
                }
                catch (...) {
                    return FailSession(
                        key, sessionGeneration, EIO, m_impl->m_connectedSocket);
                }
            }

            bool DtlsSessionManager::ArmRetransmitTimer(
                const DtlsPeerKey& key,
                std::uint64_t sessionGeneration,
                std::chrono::milliseconds delay) {
                auto* session = m_impl ? m_impl->Find(key, sessionGeneration) : nullptr;
                if (session == nullptr || m_impl->m_loop == nullptr) return false;
                m_impl->m_loop->CancelPollerTimeout(session->m_retransmitTimeoutId);
                const std::uint64_t timerGeneration = ++session->m_retransmitTimerGeneration;
                session->m_retransmitTimeoutId = m_impl->m_loop->SchedulePollerTimeout(
                    delay,
                    [this, key, sessionGeneration, timerGeneration]() {
                        HandleRetransmitTimeout(key, sessionGeneration, timerGeneration);
                    });
                return session->m_retransmitTimeoutId != Poller::InvalidTimeoutId;
            }

            bool DtlsSessionManager::ArmIdleTimer(
                const DtlsPeerKey& key,
                std::uint64_t sessionGeneration) {
                auto* session = m_impl ? m_impl->Find(key, sessionGeneration) : nullptr;
                if (session == nullptr || m_impl->m_loop == nullptr) return false;
                if (m_impl->m_idleTimeout.count() == 0) return true;
                m_impl->m_loop->CancelPollerTimeout(session->m_idleTimeoutId);
                const std::uint64_t timerGeneration = ++session->m_idleTimerGeneration;
                session->m_idleTimeoutId = m_impl->m_loop->SchedulePollerTimeout(
                    m_impl->m_idleTimeout,
                    [this, key, sessionGeneration, timerGeneration]() {
                        HandleIdleTimeout(key, sessionGeneration, timerGeneration);
                    });
                return session->m_idleTimeoutId != Poller::InvalidTimeoutId;
            }

            void DtlsSessionManager::HandleHandshakeTimeout(
                const DtlsPeerKey& key,
                std::uint64_t sessionGeneration) {
                auto* session = m_impl ? m_impl->Find(key, sessionGeneration) : nullptr;
                if (session == nullptr || session->m_closing || !session->m_engine
                    || session->m_engine->State() != DtlsState::Handshaking) return;
                session->m_handshakeTimeoutId = Poller::InvalidTimeoutId;
                m_impl->m_handshakeTimeouts.fetch_add(1, std::memory_order_relaxed);
                (void)FailSession(
                    key, sessionGeneration, ETIMEDOUT, m_impl->m_connectedSocket);
            }

            void DtlsSessionManager::HandleRetransmitTimeout(
                const DtlsPeerKey& key,
                std::uint64_t sessionGeneration,
                std::uint64_t timerGeneration) {
                auto* session = m_impl ? m_impl->Find(key, sessionGeneration) : nullptr;
                if (session == nullptr || session->m_closing || !session->m_engine
                    || !DtlsTimerGenerationMatches(
                        session->m_retransmitTimerGeneration, timerGeneration)) return;
                session->m_retransmitTimeoutId = Poller::InvalidTimeoutId;
                m_impl->m_retransmitTimeouts.fetch_add(1, std::memory_order_relaxed);
                try {
                    DtlsDatagramBatch plaintextOutput; // timer 不产生应用明文
                    DtlsDatagramBatch ciphertextOutput; // 重传 flight
                    const DtlsResult result = session->m_engine->HandleTimeout(ciphertextOutput);
                    (void)ProcessSessionResult(
                        key,
                        sessionGeneration,
                        result,
                        true,
                        plaintextOutput,
                        ciphertextOutput);
                }
                catch (...) {
                    (void)FailSession(
                        key, sessionGeneration, EIO, m_impl->m_connectedSocket);
                }
            }

            void DtlsSessionManager::HandleIdleTimeout(
                const DtlsPeerKey& key,
                std::uint64_t sessionGeneration,
                std::uint64_t timerGeneration) {
                auto* session = m_impl ? m_impl->Find(key, sessionGeneration) : nullptr;
                if (session == nullptr || session->m_closing || !session->m_engine
                    || !DtlsTimerGenerationMatches(
                        session->m_idleTimerGeneration, timerGeneration)) return;
                session->m_idleTimeoutId = Poller::InvalidTimeoutId;
                (void)FailSession(
                    key, sessionGeneration, ETIMEDOUT, m_impl->m_connectedSocket);
            }

            bool DtlsSessionManager::CreateServerSession(
                const Address& peer,
                DtlsPeerKey& key,
                std::uint64_t& sessionGeneration) {
                if (m_impl == nullptr || m_impl->m_shuttingDown) return false;
                key = DtlsSessionManagerImpl::MakeKey(peer);
                if (key.m_family == AF_UNSPEC || m_impl->Find(key) != nullptr) return false;
                const std::size_t pending = m_impl->m_pendingSessions.load(std::memory_order_acquire);
                if (!CanCreateDtlsSession(m_impl->m_limits, m_impl->m_sessions.size(), pending)) {
                    m_impl->m_droppedNewPeers.fetch_add(1, std::memory_order_relaxed);
                    return false; // 未验证 peer 超限时静默丢弃
                }
                const auto reportError = [this, &peer](int error) noexcept {
                    try { m_impl->m_connection->OnDtlsSessionError(peer, error); }
                    catch (...) {}
                    return false;
                };
                if (!m_impl->m_factory.InitializeSharedResources()) return reportError(EIO);
                std::unique_ptr<DtlsEngine> engine = m_impl->m_factory.Create(
                    peer,
                    m_impl->m_connection->GetLocalAddress(),
                    m_impl->m_maximumCiphertextDatagramBytes);
                if (!engine) return reportError(ENOMEM);

                auto session = std::make_unique<DtlsSessionManagerImpl::Session>(); // peer-local 会话
                session->m_key = key;
                session->m_peer = peer;
                session->m_local = m_impl->m_connection->GetLocalAddress();
                session->m_generation = m_impl->m_nextSessionGeneration++;
                session->m_engine = std::move(engine);
                sessionGeneration = session->m_generation;
                m_impl->m_sessions.emplace(key, std::move(session));
                m_impl->m_createdSessions.fetch_add(1, std::memory_order_relaxed);
                m_impl->m_pendingSessions.fetch_add(1, std::memory_order_release);

                auto* created = m_impl->Find(key, sessionGeneration);
                if (m_impl->m_handshakeTimeout.count() > 0) {
                    created->m_handshakeTimeoutId = m_impl->m_loop->SchedulePollerTimeout(
                        m_impl->m_handshakeTimeout,
                        [this, key, sessionGeneration]() {
                            HandleHandshakeTimeout(key, sessionGeneration);
                        });
                    if (created->m_handshakeTimeoutId == Poller::InvalidTimeoutId) {
                        return FailSession(key, sessionGeneration, EIO, false);
                    }
                }
                return true;
            }

            void DtlsSessionManager::CancelSessionTimers(
                const DtlsPeerKey& key,
                std::uint64_t sessionGeneration) noexcept {
                auto* session = m_impl ? m_impl->Find(key, sessionGeneration) : nullptr;
                if (session == nullptr || m_impl->m_loop == nullptr) return;
                m_impl->m_loop->CancelPollerTimeout(session->m_retransmitTimeoutId);
                m_impl->m_loop->CancelPollerTimeout(session->m_handshakeTimeoutId);
                m_impl->m_loop->CancelPollerTimeout(session->m_idleTimeoutId);
                session->m_retransmitTimeoutId = Poller::InvalidTimeoutId;
                session->m_handshakeTimeoutId = Poller::InvalidTimeoutId;
                session->m_idleTimeoutId = Poller::InvalidTimeoutId;
                ++session->m_retransmitTimerGeneration;
                ++session->m_idleTimerGeneration;
            }

            void DtlsSessionManager::MarkSessionClosing(
                const DtlsPeerKey& key,
                std::uint64_t sessionGeneration) noexcept {
                auto* session = m_impl ? m_impl->Find(key, sessionGeneration) : nullptr;
                if (session == nullptr || session->m_closing) return;
                CancelSessionTimers(key, sessionGeneration);
                session = m_impl->Find(key, sessionGeneration);
                if (session == nullptr) return;
                session->m_closing = true;
                if (session->m_countedPending) {
                    m_impl->m_pendingSessions.fetch_sub(1, std::memory_order_release);
                    session->m_countedPending = false;
                }
                if (session->m_countedActive) {
                    m_impl->m_activeSessions.fetch_sub(1, std::memory_order_release);
                    session->m_countedActive = false;
                }
                FinalizeSessionIfDrained(key, sessionGeneration);
            }

            void DtlsSessionManager::FinalizeSessionIfDrained(
                const DtlsPeerKey& key,
                std::uint64_t sessionGeneration) noexcept {
                auto* session = m_impl ? m_impl->Find(key, sessionGeneration) : nullptr;
                if (session == nullptr || !session->m_closing
                    || session->m_pendingCiphertextCost != 0) return;
                const Address peer(session->m_peer); // erase 后回调仍持有 sockaddr 快照
                const bool closeConnection = m_impl->m_connectedSocket;
                m_impl->m_sessions.erase(key);
                m_impl->m_closedSessions.fetch_add(1, std::memory_order_relaxed);
                try { m_impl->m_connection->OnDtlsSessionClosed(peer); }
                catch (...) {}
                if (closeConnection) m_impl->m_connection->DoClose(true);
            }

            bool DtlsSessionManager::FailSession(
                const DtlsPeerKey& key,
                std::uint64_t sessionGeneration,
                int error,
                bool closeConnection) noexcept {
                auto* session = m_impl ? m_impl->Find(key, sessionGeneration) : nullptr;
                if (session == nullptr) return false;
                const Address peer(session->m_peer); // observer 重入期间稳定 peer
                try { m_impl->m_connection->OnDtlsSessionError(peer, error); }
                catch (...) {}
                if (closeConnection) {
                    try { m_impl->m_connection->OnError(error); }
                    catch (...) {}
                }
                MarkSessionClosing(key, sessionGeneration);
                if (closeConnection) m_impl->m_connection->DoClose(true);
                return false;
            }
        }
    }
}
