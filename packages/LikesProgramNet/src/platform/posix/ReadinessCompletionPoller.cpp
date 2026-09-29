#include "net/platform/posix/ReadinessCompletionPoller.hpp"

#include "net/platform/ReadinessDriver.hpp"
#include "net/platform/SocketOps.hpp"
#include "net/PollerAccess.hpp"
#include "net/DatagramCompletionPolicy.hpp"

#include <LikesProgram/Net/Address.hpp>
#include <LikesProgram/Net/Buffer.hpp>
#include <LikesProgram/Net/BufferChain.hpp>
#include <LikesProgram/Net/BufferSlice.hpp>
#include <LikesProgram/Net/Connection.hpp>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdint>
#include <deque>
#include <limits>
#include <netinet/in.h>
#include <queue>
#include <sys/socket.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace LikesProgram {
    namespace Net {
        namespace Internal {
            enum class ReadinessRegistrationKind {
                Channel,
                Connect,
                Accept,
                Connection
            };

            struct ReadinessRegistration {
                ReadinessRegistrationId m_id = 0; // Channel 的稳定逻辑身份
                ReadinessRegistrationKind m_kind = ReadinessRegistrationKind::Channel; // completion 分派类型
                SocketType m_fd = kInvalidSocket; // driver 操作使用的当前 fd
                Channel* m_channel = nullptr; // 非拥有的 Channel 观察指针
                Poller::ConnectId m_operationId = Poller::InvalidConnectId; // connect 状态定位键
                Connection* m_connectionKey = nullptr; // TCP Connection 状态定位键
            };

            struct ReadinessConnectState {
                Poller::ConnectId m_id = Poller::InvalidConnectId; // 调用方取消使用的稳定 id
                ReadinessRegistrationId m_registrationId = 0; // readiness registration 身份
                SocketType m_fd = kInvalidSocket; // callback 前由 common Poller 独占
                Poller::ConnectCallback m_callback; // issuer 线程 completion 入口
                bool m_readyToComplete = false; // 立即结果等待 Flush 延迟分派
                int m_error = 0; // 立即失败或 SO_ERROR 结果
            };

            struct ReadinessAcceptState {
                SocketType m_fd = kInvalidSocket; // listener 仅观察，不拥有生命周期
                ReadinessRegistrationId m_registrationId = 0; // listener readiness 身份
                Poller::AcceptCallback m_callback; // accepted fd 消费入口
            };

            struct ReadinessDatagramWriteState {
                ReadinessDatagramWriteState(
                    DatagramWriteTarget target,
                    const Address& peer,
                    Buffer&& buffer,
                    std::size_t queueCost)
                    : m_target(target),
                    m_peer(peer),
                    m_buffer(std::move(buffer)),
                    m_queueCost(queueCost) {
                }

                DatagramWriteTarget m_target = DatagramWriteTarget::Invalid; // connected 或显式 peer
                Address m_peer; // DTLS 回收与 sendto 使用的原始 peer
                Buffer m_buffer{ 0 }; // 完成或关闭前独占完整 payload
                std::size_t m_queueCost = 0; // 空包也占一个背压单位
            };

            struct ReadinessConnectionState {
                std::shared_ptr<Connection> m_connection; // callback 期间保持对象存活
                ReadinessRegistrationId m_registrationId = 0; // fd 复用时不复用的身份
                SocketType m_fd = kInvalidSocket; // Connection 持有最终关闭责任
                bool m_datagram = false; // UDP 使用数据报边界，TCP 使用字节流
                bool m_readEnabled = true; // 业务背压控制的读开关
                bool m_closing = false; // 关闭后不再 drain
                bool m_writeFlushQueued = false; // 防止重复进入 Flush 队列
                std::size_t m_pendingWriteBytes = 0; // BufferChain 未发送字节数
                std::size_t m_maxDatagramBytes = 0; // Start 时固定的单包接收容量
                BufferChain m_writeChain; // partial send 前保持全部段所有权
                std::deque<ReadinessDatagramWriteState> m_datagramWrites; // UDP FIFO
            };

            struct ReadinessWriteFlushItem {
                Connection* m_connectionKey = nullptr; // 只用于重新查表
                ReadinessRegistrationId m_registrationId = 0; // 地址复用时拒绝 stale item
            };

            struct ReadinessTimeoutState {
                Poller::TimeoutId m_id = Poller::InvalidTimeoutId; // 当前 timer generation
                std::chrono::steady_clock::time_point m_deadline{}; // 单调绝对到期点
                Poller::TimeoutCallback m_callback; // 到期时执行一次的回调
            };

            struct ReadinessTimeoutHeapNode {
                std::chrono::steady_clock::time_point m_deadline{}; // heap 排序主键
                Poller::TimeoutId m_id = Poller::InvalidTimeoutId; // 与 active 状态对账
            };

            struct ReadinessTimeoutHeapCompare {
                // 让 heap 顶部保持最早 deadline 和最小 id。
                bool operator()(
                    const ReadinessTimeoutHeapNode& left,
                    const ReadinessTimeoutHeapNode& right) const noexcept {
                    if (left.m_deadline != right.m_deadline) {
                        return left.m_deadline > right.m_deadline;
                    }
                    return left.m_id > right.m_id;
                }
            };

            struct ReadinessCompletionPoller::ReadinessCompletionPollerImpl {
                EventLoop* m_ownerLoop = nullptr; // common Poller 所属 issuer
                std::unique_ptr<ReadinessDriver> m_driver; // 当前平台 readiness driver
                bool m_activated = false; // driver 是否已完成激活
                ReadinessRegistrationId m_nextRegistrationId = 1; // 单调 registration id
                Poller::ConnectId m_nextConnectId = 1; // 单调 connect operation id
                Poller::TimeoutId m_nextTimeoutId = 1; // 单调 timer generation id
                std::unordered_map<ReadinessRegistrationId,
                    ReadinessRegistration> m_registrations; // registration 快照
                std::unordered_map<Channel*, ReadinessRegistrationId>
                    m_channelRegistrationIds; // Channel 到 registration 的反向索引
                std::unordered_map<Poller::ConnectId, ReadinessConnectState>
                    m_connectStates; // connect socket 与 callback 所有权
                std::unordered_map<SocketType, ReadinessAcceptState>
                    m_acceptStates; // listener completion 状态
                std::unordered_map<Connection*, ReadinessConnectionState>
                    m_connectionStates; // TCP completion 状态
                std::deque<ReadinessWriteFlushItem> m_writeFlushQueue; // QueueWrite 到 Flush 队列
                std::unordered_map<Poller::TimeoutId, ReadinessTimeoutState>
                    m_timeouts; // 当前 active timer 状态
                std::priority_queue<ReadinessTimeoutHeapNode,
                    std::vector<ReadinessTimeoutHeapNode>,
                    ReadinessTimeoutHeapCompare> m_timeoutHeap; // 允许保留 stale node
                std::vector<ReadinessEvent> m_readyEvents; // 单轮 driver 事件快照
                std::uint64_t m_completedOperations = 0; // 已分发的 Channel/timer completion
                std::uint64_t m_completionBatchCount = 0; // 非空 Poll 批次数
                std::uint64_t m_completionBatchItems = 0; // 全部 Poll completion 数量
                std::uint64_t m_peakCompletionBatch = 0; // 单轮 completion 峰值
                std::uint64_t m_readSubmissions = 0; // 启用 TCP read interest 次数
                std::uint64_t m_receivedBytes = 0; // TCP read completion 总字节数
                std::uint64_t m_receiveCompletions = 0; // owning Buffer read 次数
                std::uint64_t m_datagramReceiveCompletions = 0; // 成功 recvfrom 次数
                std::uint64_t m_datagramSendCompletions = 0; // 成功 send/sendto 次数
                std::uint64_t m_receivedDatagrams = 0; // 已交付业务的数据报数
                std::uint64_t m_sentDatagrams = 0; // 已完整发送的数据报数
                std::uint64_t m_zeroLengthDatagrams = 0; // 收发空数据报总数
                std::uint64_t m_truncatedDatagrams = 0; // 接收容量不足的数据报数
                std::size_t m_pendingDatagramSends = 0; // FIFO 内数据报节点数
                std::uint64_t m_datagramSendBatchSubmissions = 0; // 单数据报 syscall 数
                std::size_t m_maximumDatagramSendBatch = 0; // readiness 固定为 1

                // 分配不与活动 registration 冲突的非零身份。
                ReadinessRegistrationId NextRegistrationId() noexcept {
                    ReadinessRegistrationId registrationId = m_nextRegistrationId++;
                    while (registrationId == 0
                        || m_registrations.find(registrationId) != m_registrations.end()) {
                        registrationId = m_nextRegistrationId++;
                    }
                    return registrationId;
                }

                // 分配不与 active connect 冲突的非零 operation id。
                Poller::ConnectId NextConnectId() noexcept {
                    Poller::ConnectId connectId = m_nextConnectId++;
                    while (connectId == Poller::InvalidConnectId
                        || m_connectStates.find(connectId) != m_connectStates.end()) {
                        connectId = m_nextConnectId++;
                    }
                    return connectId;
                }

                // 分配不与 active timer 冲突的非零身份。
                Poller::TimeoutId NextTimeoutId() noexcept {
                    Poller::TimeoutId timeoutId = m_nextTimeoutId++;
                    while (timeoutId == Poller::InvalidTimeoutId
                        || m_timeouts.find(timeoutId) != m_timeouts.end()) {
                        timeoutId = m_nextTimeoutId++;
                    }
                    return timeoutId;
                }

                // 丢弃已取消或 generation 已变化的 heap 节点。
                void PruneTimeoutHeap() noexcept {
                    while (!m_timeoutHeap.empty()) {
                        const ReadinessTimeoutHeapNode& node = m_timeoutHeap.top(); // 最早候选
                        const auto found = m_timeouts.find(node.m_id);
                        if (found != m_timeouts.end()
                            && found->second.m_deadline == node.m_deadline) {
                            return;
                        }
                        m_timeoutHeap.pop();
                    }
                }

                // 将绝对 deadline 转为 driver 的有界毫秒等待预算。
                int WaitMillisecondsUntil(
                    std::chrono::steady_clock::time_point deadline) const noexcept {
                    const auto now = std::chrono::steady_clock::now(); // EINTR 后重新读取时钟
                    if (deadline <= now) return 0;

                    const auto remaining = deadline - now; // 保持单一绝对 deadline
                    const auto rounded = std::chrono::ceil<std::chrono::milliseconds>(remaining);
                    if (rounded.count() >= INT_MAX) return INT_MAX;
                    return static_cast<int>(rounded.count());
                }

                // 记录一轮至少产生一个 completion 的批次。
                void RecordCompletionBatch(std::uint64_t batchItems) noexcept {
                    if (batchItems == 0) return;
                    ++m_completionBatchCount;
                    m_completionBatchItems += batchItems;
                    m_peakCompletionBatch = std::max(m_peakCompletionBatch, batchItems);
                }
            };

            ReadinessCompletionPoller::ReadinessCompletionPoller(
                EventLoop* ownerLoop,
                std::unique_ptr<ReadinessDriver> driver)
                : Poller(ownerLoop), m_impl(new ReadinessCompletionPollerImpl{}) {
                // shell 阶段只保存 issuer 与注入的 driver，不接管生产 factory。
                m_impl->m_ownerLoop = ownerLoop;
                m_impl->m_driver = std::move(driver);
            }

            ReadinessCompletionPoller::~ReadinessCompletionPoller() {
                // Poller 先于 Connection 失效时解除回调关系，避免析构重入。
                if (m_impl != nullptr) {
                    for (auto& item : m_impl->m_connectionStates) {
                        if (item.second.m_connection) {
                            PollerAccess::Detach(*item.second.m_connection);
                        }
                    }
                    m_impl->m_connectionStates.clear();
                    m_impl->m_writeFlushQueue.clear();
                }
                // 释放 driver 与 common PImpl，避免析构后访问平台状态。
                delete m_impl;
                m_impl = nullptr;
            }

            bool ReadinessCompletionPoller::Activate() {
                // 只在 issuer 线程激活一次注入的 readiness driver。
                if (m_impl == nullptr || m_impl->m_driver == nullptr) return false;
                if (m_impl->m_activated) return true;
                m_impl->m_activated = m_impl->m_driver->Activate();
                if (!m_impl->m_activated) {
                    SetLastError(m_impl->m_driver->LastError());
                }
                return m_impl->m_activated;
            }

            void ReadinessCompletionPoller::RemoveRegistration(
                std::uint64_t registrationId) noexcept {
                // 先抹掉逻辑身份，再撤销平台 registration，屏蔽迟到事件。
                if (m_impl == nullptr || registrationId == 0) return;
                const auto found = m_impl->m_registrations.find(registrationId);
                if (found == m_impl->m_registrations.end()) return;
                const SocketType fd = found->second.m_fd; // erase 前固定 driver 目标
                m_impl->m_registrations.erase(found);
                if (m_impl->m_driver == nullptr
                    || m_impl->m_driver->Remove(registrationId, fd)) {
                    return;
                }
                const int error = m_impl->m_driver->LastError(); // 逻辑身份已撤销
                if (error != ENOENT && error != EBADF) SetLastError(error);
            }

            void ReadinessCompletionPoller::CompleteConnect(
                ConnectId connectId,
                int error) noexcept {
                // callback 前移除 state 与 registration，避免回调重入再次完成。
                if (m_impl == nullptr || connectId == InvalidConnectId) return;
                const auto found = m_impl->m_connectStates.find(connectId);
                if (found == m_impl->m_connectStates.end()) return;
                const ReadinessRegistrationId registrationId = found->second.m_registrationId;
                SocketType completedFd = found->second.m_fd; // 成功时移交 callback
                ConnectCallback callback = std::move(found->second.m_callback);
                RemoveRegistration(registrationId);
                m_impl->m_connectStates.erase(found);
                ++m_impl->m_completedOperations;

                if (error != 0) {
                    Internal::CloseSocket(completedFd);
                    completedFd = kInvalidSocket;
                }
                try {
                    if (callback) callback(completedFd, error);
                }
                catch (...) {
                    // 用户成功 callback 抛出时收回仍未释放的 socket。
                    if (error == 0) Internal::CloseSocket(completedFd);
                }
            }

            std::uint64_t ReadinessCompletionPoller::DrainAccept(
                SocketType listenFd,
                std::uint64_t registrationId) noexcept {
                // 一次 readiness 窗口持续 accept 到 EAGAIN，保持 level-triggered 语义。
                if (m_impl == nullptr || listenFd == kInvalidSocket || registrationId == 0) {
                    return 0;
                }
                std::uint64_t completed = 0; // 当前 readiness 实际接受的连接数
                while (true) {
                    const auto found = m_impl->m_acceptStates.find(listenFd);
                    if (found == m_impl->m_acceptStates.end()
                        || found->second.m_registrationId != registrationId) {
                        break;
                    }
                    const SocketType clientFd = Internal::AcceptSocket(
                        listenFd,
                        nullptr,
                        nullptr); // 每个成功 fd 形成一个 completion
                    if (clientFd != kInvalidSocket) {
                        AcceptCallback callback; // callback 可能 StopAccept 并删除 state
                        try {
                            callback = found->second.m_callback;
                        }
                        catch (...) {
                            Internal::CloseSocket(clientFd);
                            SetLastError(ENOMEM);
                            break;
                        }
                        try {
                            if (callback) callback(clientFd);
                            else Internal::CloseSocket(clientFd);
                        }
                        catch (...) {
                            // 用户 accept 入口失败时关闭 fd，避免 completion 泄漏。
                            Internal::CloseSocket(clientFd);
                        }
                        ++completed;
                        ++m_impl->m_completedOperations;
                        continue;
                    }

                    const int error = Internal::GetLastSocketError(); // accept4 的稳定 errno
                    if (Internal::IsInterrupted(error)) continue;
                    if (Internal::IsWouldBlock(error)) break;
                    if (error != EMFILE && error != ENFILE
                        && error != ENOBUFS && error != ENOMEM) {
                        SetLastError(error);
                    }
                    break;
                }
                return completed;
            }

            bool ReadinessCompletionPoller::UpdateConnectionInterest(
                Connection* connection) noexcept {
                // interest 由读开关和待写链共同决定，错误/关闭始终保留。
                if (m_impl == nullptr || connection == nullptr) return false;
                const auto found = m_impl->m_connectionStates.find(connection);
                if (found == m_impl->m_connectionStates.end() || found->second.m_closing) {
                    return false;
                }
                ReadinessConnectionState& state = found->second; // issuer 独占快照
                IOEvent events = IOEvent::Error | IOEvent::Close; // 始终观察错误与半关闭
                if (state.m_readEnabled) events |= IOEvent::Read;
                const bool writePending = state.m_datagram
                    ? !state.m_datagramWrites.empty()
                    : !state.m_writeChain.Empty();
                if (writePending) events |= IOEvent::Write;
                if (m_impl->m_driver->Modify(
                    state.m_registrationId,
                    state.m_fd,
                    events)) {
                    return true;
                }
                SetLastError(m_impl->m_driver->LastError());
                return false;
            }

            std::uint64_t ReadinessCompletionPoller::DrainConnectionRead(
                Connection* connection,
                std::uint64_t registrationId,
                bool drainForClose) noexcept {
                // 有界 drain 保持 payload 先于 EOF，并避免单连接独占 issuer。
                if (m_impl == nullptr || connection == nullptr || registrationId == 0) return 0;
                constexpr std::size_t kReadCapacity = 64 * 1024; // 单次 owning Buffer 上界
                constexpr int kMaximumDrainIterations = 64; // 单轮公平上界
                std::uint64_t completed = 0; // read/EOF/error completion 数
                for (int iteration = 0; iteration < kMaximumDrainIterations; ++iteration) {
                    const auto found = m_impl->m_connectionStates.find(connection);
                    if (found == m_impl->m_connectionStates.end()
                        || found->second.m_registrationId != registrationId
                        || found->second.m_closing
                        || (!found->second.m_readEnabled && !drainForClose)) {
                        break;
                    }
                    const SocketType fd = found->second.m_fd; // callback 前固定 fd
                    std::shared_ptr<Connection> snapshot = found->second.m_connection; // callback 保活
                    try {
                        Buffer input(0); // socket 直接写入 owning Buffer
                        std::uint8_t* destination = input.PrepareWrite(kReadCapacity);
                        const std::int64_t received = Internal::ReceiveSocket(
                            fd,
                            destination,
                            kReadCapacity,
                            0);
                        if (received > 0) {
                            const std::size_t receivedBytes = static_cast<std::size_t>(received);
                            input.HasWritten(receivedBytes);
                            m_impl->m_receivedBytes += receivedBytes;
                            ++m_impl->m_receiveCompletions;
                            ++m_impl->m_completedOperations;
                            ++completed;
                            PollerAccess::CompleteRead(*snapshot, std::move(input));
                            continue;
                        }
                        if (received == 0) {
                            ++m_impl->m_completedOperations;
                            ++completed;
                            PollerAccess::PeerClosed(*snapshot);
                            break;
                        }
                        const int error = Internal::GetLastSocketError(); // recv 的稳定 errno
                        if (Internal::IsInterrupted(error)) continue;
                        if (Internal::IsWouldBlock(error)) break;
                        ++m_impl->m_completedOperations;
                        ++completed;
                        PollerAccess::Error(*snapshot, error);
                        break;
                    }
                    catch (...) {
                        PollerAccess::Error(*snapshot, ENOMEM);
                        ++m_impl->m_completedOperations;
                        ++completed;
                        break;
                    }
                }
                return completed;
            }

            std::uint64_t ReadinessCompletionPoller::DrainConnectionWrite(
                Connection* connection,
                std::uint64_t registrationId) noexcept {
                // 按 BufferChain FIFO 有界发送，partial send 只消费完成字节。
                if (m_impl == nullptr || connection == nullptr || registrationId == 0) return 0;
                constexpr int kMaximumDrainIterations = 64; // partial send 公平上界
                std::uint64_t completed = 0; // send/error completion 数
                for (int iteration = 0; iteration < kMaximumDrainIterations; ++iteration) {
                    const auto found = m_impl->m_connectionStates.find(connection);
                    if (found == m_impl->m_connectionStates.end()
                        || found->second.m_registrationId != registrationId
                        || found->second.m_closing
                        || found->second.m_writeChain.Empty()) {
                        break;
                    }
                    ReadinessConnectionState& state = found->second; // syscall 前不执行 callback
                    const BufferSlice segment = state.m_writeChain.Segment(0); // FIFO 头借用视图
                    const std::int64_t sent = Internal::SendSocket(
                        state.m_fd,
                        segment.Data(),
                        segment.Size(),
                        0);
                    if (sent > 0) {
                        const std::size_t sentBytes = static_cast<std::size_t>(sent);
                        state.m_writeChain.Consume(sentBytes);
                        state.m_pendingWriteBytes -= std::min(sentBytes, state.m_pendingWriteBytes);
                        const std::size_t pendingBytes = state.m_pendingWriteBytes; // callback 快照
                        std::shared_ptr<Connection> snapshot = state.m_connection; // callback 保活
                        ++m_impl->m_completedOperations;
                        ++completed;
                        PollerAccess::WriteDrain(*snapshot, pendingBytes);
                        const auto current = m_impl->m_connectionStates.find(connection);
                        if (current == m_impl->m_connectionStates.end()
                            || current->second.m_registrationId != registrationId) {
                            break;
                        }
                        if (current->second.m_writeChain.Empty()) {
                            if (!UpdateConnectionInterest(connection)) {
                                PollerAccess::Error(*snapshot, EIO);
                                break;
                            }
                            PollerAccess::WriteComplete(*snapshot);
                            break;
                        }
                        continue;
                    }

                    const int error = sent == 0 ? EIO : Internal::GetLastSocketError();
                    if (sent < 0 && Internal::IsInterrupted(error)) continue;
                    if (sent < 0 && Internal::IsWouldBlock(error)) {
                        if (!UpdateConnectionInterest(connection)) {
                            std::shared_ptr<Connection> snapshot = state.m_connection;
                            PollerAccess::Error(*snapshot, EIO);
                        }
                        break;
                    }
                    std::shared_ptr<Connection> snapshot = state.m_connection; // Error 会删除 state
                    ++m_impl->m_completedOperations;
                    ++completed;
                    PollerAccess::Error(*snapshot, error);
                    break;
                }
                const auto remaining = m_impl->m_connectionStates.find(connection);
                if (remaining != m_impl->m_connectionStates.end()
                    && remaining->second.m_registrationId == registrationId
                    && !remaining->second.m_closing
                    && !remaining->second.m_writeChain.Empty()) {
                    std::shared_ptr<Connection> snapshot = remaining->second.m_connection;
                    if (!UpdateConnectionInterest(connection)) PollerAccess::Error(*snapshot, EIO);
                }
                return completed;
            }

            std::uint64_t ReadinessCompletionPoller::DrainDatagramRead(
                Connection* connection,
                std::uint64_t registrationId) noexcept {
                // 一轮有界 drain 保持数据报边界、peer 与截断元数据。
                if (m_impl == nullptr || connection == nullptr || registrationId == 0) return 0;
                constexpr int kMaximumDrainIterations = 64; // 限制单连接占用
                std::uint64_t completed = 0; // 数据报或错误 completion 数
                for (int iteration = 0; iteration < kMaximumDrainIterations; ++iteration) {
                    const auto found = m_impl->m_connectionStates.find(connection);
                    if (found == m_impl->m_connectionStates.end()
                        || found->second.m_registrationId != registrationId
                        || !found->second.m_datagram
                        || found->second.m_closing
                        || !found->second.m_readEnabled) {
                        break;
                    }
                    const SocketType fd = found->second.m_fd; // callback 前固定 fd
                    const std::size_t capacity = found->second.m_maxDatagramBytes; // Start 快照
                    std::shared_ptr<Connection> snapshot = found->second.m_connection; // callback 保活
                    try {
                        Buffer input(0); // readiness 数据报使用 owning Buffer
                        std::uint8_t* destination = input.PrepareWrite(capacity);
                        sockaddr_storage peerStorage{}; // 当前数据报 peer
                        SocketLength peerLength = static_cast<SocketLength>(sizeof(peerStorage));
                        const std::int64_t received = Internal::ReceiveSocketFrom(
                            fd,
                            destination,
                            capacity,
                            MSG_TRUNC,
                            reinterpret_cast<sockaddr*>(&peerStorage),
                            &peerLength);
                        if (received >= 0) {
                            const DatagramReceiveResult result = InterpretDatagramReceive(
                                static_cast<std::size_t>(received),
                                capacity,
                                false);
                            input.HasWritten(result.payloadBytes);
                            Address peer(peerStorage, peerLength); // 保留原始 peer
                            ++m_impl->m_datagramReceiveCompletions;
                            ++m_impl->m_receivedDatagrams;
                            if (result.originalBytes == 0) ++m_impl->m_zeroLengthDatagrams;
                            if (result.truncated) ++m_impl->m_truncatedDatagrams;
                            ++m_impl->m_completedOperations;
                            ++completed;
                            PollerAccess::CompleteDatagram(
                                *snapshot,
                                input,
                                peer,
                                result.originalBytes,
                                result.truncated);
                            continue;
                        }
                        const int error = Internal::GetLastSocketError(); // recvfrom errno
                        if (Internal::IsInterrupted(error)) continue;
                        if (Internal::IsWouldBlock(error)) break;
                        ++m_impl->m_completedOperations;
                        ++completed;
                        PollerAccess::Error(*snapshot, error);
                        break;
                    }
                    catch (...) {
                        ++m_impl->m_completedOperations;
                        ++completed;
                        PollerAccess::Error(*snapshot, ENOMEM);
                        break;
                    }
                }
                return completed;
            }

            std::uint64_t ReadinessCompletionPoller::DrainDatagramWrite(
                Connection* connection,
                std::uint64_t registrationId) noexcept {
                // 每次 syscall 发送一个完整数据报，绝不 partial consume payload。
                if (m_impl == nullptr || connection == nullptr || registrationId == 0) return 0;
                constexpr int kMaximumDrainIterations = 16; // 给关闭任务留下 FIFO 尾部
                std::uint64_t completed = 0; // 完整 send 或 fatal error 数
                for (int iteration = 0; iteration < kMaximumDrainIterations; ++iteration) {
                    const auto found = m_impl->m_connectionStates.find(connection);
                    if (found == m_impl->m_connectionStates.end()
                        || found->second.m_registrationId != registrationId
                        || !found->second.m_datagram
                        || found->second.m_closing
                        || found->second.m_datagramWrites.empty()) {
                        break;
                    }
                    ReadinessConnectionState& state = found->second; // syscall 前不执行 callback
                    ReadinessDatagramWriteState& head = state.m_datagramWrites.front();
                    const std::size_t payloadBytes = head.m_buffer.ReadableBytes();
                    ++m_impl->m_datagramSendBatchSubmissions;
                    m_impl->m_maximumDatagramSendBatch = 1;
                    std::int64_t sent = -1; // 当前单数据报 syscall 结果
                    if (head.m_target == DatagramWriteTarget::Connected) {
                        sent = Internal::SendSocket(
                            state.m_fd,
                            head.m_buffer.Peek(),
                            payloadBytes,
                            0);
                    }
                    else if (head.m_target == DatagramWriteTarget::ExplicitPeer) {
                        sent = Internal::SendSocketTo(
                            state.m_fd,
                            head.m_buffer.Peek(),
                            payloadBytes,
                            0,
                            head.m_peer.SockAddr(),
                            head.m_peer.Length());
                    }
                    else {
                        errno = EDESTADDRREQ;
                    }

                    if (sent >= 0 && DatagramWriteCompleted(
                        static_cast<std::size_t>(sent), payloadBytes)) {
                        ReadinessDatagramWriteState completedWrite = std::move(head);
                        state.m_datagramWrites.pop_front();
                        state.m_pendingWriteBytes -= std::min(
                            completedWrite.m_queueCost,
                            state.m_pendingWriteBytes);
                        if (m_impl->m_pendingDatagramSends > 0) --m_impl->m_pendingDatagramSends;
                        const std::size_t pendingBytes = state.m_pendingWriteBytes;
                        std::shared_ptr<Connection> snapshot = state.m_connection;
                        ++m_impl->m_datagramSendCompletions;
                        ++m_impl->m_sentDatagrams;
                        if (payloadBytes == 0) ++m_impl->m_zeroLengthDatagrams;
                        ++m_impl->m_completedOperations;
                        ++completed;
                        PollerAccess::WriteDrain(*snapshot, pendingBytes);
                        PollerAccess::DatagramWriteCompleted(
                            *snapshot,
                            completedWrite.m_peer,
                            completedWrite.m_queueCost);
                        const auto current = m_impl->m_connectionStates.find(connection);
                        if (current == m_impl->m_connectionStates.end()
                            || current->second.m_registrationId != registrationId) {
                            break;
                        }
                        if (current->second.m_datagramWrites.empty()) {
                            if (!UpdateConnectionInterest(connection)) {
                                PollerAccess::Error(*snapshot, EIO);
                                break;
                            }
                            PollerAccess::WriteComplete(*snapshot);
                            break;
                        }
                        continue;
                    }

                    const int error = sent < 0 ? Internal::GetLastSocketError() : EIO;
                    if (sent < 0 && Internal::IsInterrupted(error)) continue;
                    if (sent < 0 && Internal::IsWouldBlock(error)) {
                        std::shared_ptr<Connection> snapshot = state.m_connection;
                        if (!UpdateConnectionInterest(connection)) PollerAccess::Error(*snapshot, EIO);
                        break;
                    }
                    std::shared_ptr<Connection> snapshot = state.m_connection; // Error 前回收成本
                    std::deque<ReadinessDatagramWriteState> abandoned;
                    abandoned.swap(state.m_datagramWrites);
                    const std::size_t abandonedCount = abandoned.size();
                    state.m_pendingWriteBytes = 0;
                    m_impl->m_pendingDatagramSends = abandonedCount >= m_impl->m_pendingDatagramSends
                        ? 0
                        : m_impl->m_pendingDatagramSends - abandonedCount;
                    for (const ReadinessDatagramWriteState& datagram : abandoned) {
                        PollerAccess::DatagramWriteCompleted(
                            *snapshot,
                            datagram.m_peer,
                            datagram.m_queueCost);
                    }
                    ++m_impl->m_completedOperations;
                    ++completed;
                    PollerAccess::Error(*snapshot, error);
                    break;
                }
                const auto remaining = m_impl->m_connectionStates.find(connection);
                if (remaining != m_impl->m_connectionStates.end()
                    && remaining->second.m_registrationId == registrationId
                    && !remaining->second.m_closing
                    && !remaining->second.m_datagramWrites.empty()) {
                    std::shared_ptr<Connection> snapshot = remaining->second.m_connection;
                    if (!UpdateConnectionInterest(connection)) PollerAccess::Error(*snapshot, EIO);
                }
                return completed;
            }

            bool ReadinessCompletionPoller::AddChannel(Channel* channel) {
                // 未激活、空指针和无效 fd 不进入 common registration。
                if (m_impl == nullptr || !m_impl->m_activated
                    || channel == nullptr || channel->GetSocket() == kInvalidSocket) {
                    return false;
                }
                const auto existing = m_impl->m_channelRegistrationIds.find(channel);
                if (existing != m_impl->m_channelRegistrationIds.end()) {
                    return UpdateChannel(channel);
                }
                if (!StoreChannel(channel)) return false;

                const ReadinessRegistrationId registrationId =
                    m_impl->NextRegistrationId(); // fd 复用时不复用逻辑身份
                ReadinessRegistration registration{}; // driver 调用前完整建立快照
                registration.m_id = registrationId;
                registration.m_kind = ReadinessRegistrationKind::Channel;
                registration.m_fd = channel->GetSocket();
                registration.m_channel = channel;
                try {
                    m_impl->m_registrations.emplace(registrationId, registration);
                    m_impl->m_channelRegistrationIds.emplace(channel, registrationId);
                }
                catch (...) {
                    m_impl->m_registrations.erase(registrationId);
                    m_impl->m_channelRegistrationIds.erase(channel);
                    (void)EraseChannel(channel);
                    SetLastError(ENOMEM);
                    return false;
                }

                if (!m_impl->m_driver->Add(
                    registrationId,
                    registration.m_fd,
                    channel->Events())) {
                    // driver 拒绝时回滚所有身份，避免留下不可更新的半 registration。
                    const int error = m_impl->m_driver->LastError();
                    m_impl->m_channelRegistrationIds.erase(channel);
                    m_impl->m_registrations.erase(registrationId);
                    (void)EraseChannel(channel);
                    SetLastError(error);
                    return false;
                }
                channel->SetIndex(Channel::Index::Added);
                return true;
            }

            bool ReadinessCompletionPoller::RemoveChannel(Channel* channel) {
                // 先从 common 身份表删除，再让 driver 撤销平台 registration。
                if (m_impl == nullptr || channel == nullptr) return false;
                const auto foundId = m_impl->m_channelRegistrationIds.find(channel);
                if (foundId == m_impl->m_channelRegistrationIds.end()) return false;

                const ReadinessRegistrationId registrationId = foundId->second; // 迟到事件随后失效
                const auto foundRegistration = m_impl->m_registrations.find(registrationId);
                const SocketType fd = foundRegistration == m_impl->m_registrations.end()
                    ? channel->GetSocket()
                    : foundRegistration->second.m_fd;
                m_impl->m_channelRegistrationIds.erase(foundId);
                m_impl->m_registrations.erase(registrationId);
                (void)EraseChannel(channel);
                channel->SetIndex(Channel::Index::Deleted);
                if (m_impl->m_driver->Remove(registrationId, fd)) return true;

                const int error = m_impl->m_driver->LastError(); // 逻辑身份已先撤销
                if (error != ENOENT && error != EBADF) SetLastError(error);
                return error == ENOENT || error == EBADF;
            }

            bool ReadinessCompletionPoller::UpdateChannel(Channel* channel) {
                // 更新只修改事件位，保持 registration id 不变。
                if (m_impl == nullptr || !m_impl->m_activated || channel == nullptr) return false;
                const auto foundId = m_impl->m_channelRegistrationIds.find(channel);
                if (foundId == m_impl->m_channelRegistrationIds.end()) return AddChannel(channel);
                const auto foundRegistration = m_impl->m_registrations.find(foundId->second);
                if (foundRegistration == m_impl->m_registrations.end()) return false;
                if (m_impl->m_driver->Modify(
                    foundId->second,
                    foundRegistration->second.m_fd,
                    channel->Events())) {
                    return true;
                }
                SetLastError(m_impl->m_driver->LastError());
                return false;
            }

            void ReadinessCompletionPoller::Poll(int timeoutMs, std::vector<Channel*>& active) {
                active.clear();
                if (m_impl == nullptr || !m_impl->m_activated || m_impl->m_driver == nullptr) {
                    return;
                }

                // 先分派立即完成的 connect，避免同步结果被等待预算掩盖。
                Flush();

                // timer 只能收窄调用方等待预算，EINTR 始终复用同一个绝对 deadline。
                const auto callerDeadline = std::chrono::steady_clock::now()
                    + std::chrono::milliseconds(std::max(timeoutMs, 0));
                m_impl->PruneTimeoutHeap();
                auto waitDeadline = callerDeadline; // 当前 Poll 的默认 deadline
                if (!m_impl->m_timeoutHeap.empty()) {
                    waitDeadline = std::min(waitDeadline, m_impl->m_timeoutHeap.top().m_deadline);
                }

                int eventCount = 0; // driver 返回的逻辑事件数量
                while (true) {
                    const int waitMilliseconds = m_impl->WaitMillisecondsUntil(waitDeadline);
                    m_impl->m_readyEvents.clear();
                    eventCount = m_impl->m_driver->Wait(
                        waitMilliseconds,
                        m_impl->m_readyEvents);
                    if (eventCount >= 0) break;
                    const int error = m_impl->m_driver->LastError(); // EINTR 不重置 deadline
                    if (error == EINTR
                        && std::chrono::steady_clock::now() < waitDeadline) {
                        continue;
                    }
                    SetLastError(error);
                    return;
                }

                std::uint64_t batchItems = 0; // 当前轮真正匹配 registration 的 completion 数
                const std::size_t readyCount = std::min(
                    static_cast<std::size_t>(eventCount),
                    m_impl->m_readyEvents.size());
                for (std::size_t index = 0; index < readyCount; ++index) {
                    const ReadinessEvent& event = m_impl->m_readyEvents[index]; // 稳定事件快照
                    const auto found = m_impl->m_registrations.find(event.registrationId);
                    if (found == m_impl->m_registrations.end()) continue;
                    const ReadinessRegistration registration = found->second; // callback 前固定身份
                    if (registration.m_kind == ReadinessRegistrationKind::Channel) {
                        Channel* channel = registration.m_channel; // 非拥有的 Channel 观察指针
                        if (channel == nullptr) continue;
                        channel->SetRevents(event.events);
                        active.push_back(channel);
                        ++m_impl->m_completedOperations;
                        ++batchItems;
                        continue;
                    }
                    if (registration.m_kind == ReadinessRegistrationKind::Connect) {
                        const auto connect = m_impl->m_connectStates.find(registration.m_operationId);
                        if (connect == m_impl->m_connectStates.end()
                            || connect->second.m_registrationId != registration.m_id) {
                            continue;
                        }
                        const int error = Internal::GetSocketPendingError(connect->second.m_fd);
                        CompleteConnect(registration.m_operationId, error);
                        ++batchItems;
                        continue;
                    }
                    if (registration.m_kind == ReadinessRegistrationKind::Accept) {
                        batchItems += DrainAccept(registration.m_fd, registration.m_id);
                        continue;
                    }
                    if (registration.m_kind == ReadinessRegistrationKind::Connection) {
                        const auto connection = m_impl->m_connectionStates.find(
                            registration.m_connectionKey);
                        if (connection == m_impl->m_connectionStates.end()
                            || connection->second.m_registrationId != registration.m_id) {
                            continue;
                        }
                        const bool closeReady = HasReadinessEvent(event.events, IOEvent::Close);
                        if (HasReadinessEvent(event.events, IOEvent::Read) || closeReady) {
                            batchItems += connection->second.m_datagram
                                ? DrainDatagramRead(
                                    registration.m_connectionKey,
                                    registration.m_id)
                                : DrainConnectionRead(
                                    registration.m_connectionKey,
                                    registration.m_id,
                                    closeReady);
                        }
                        const auto afterRead = m_impl->m_connectionStates.find(
                            registration.m_connectionKey);
                        if (afterRead == m_impl->m_connectionStates.end()
                            || afterRead->second.m_registrationId != registration.m_id) {
                            continue;
                        }
                        if (HasReadinessEvent(event.events, IOEvent::Error)) {
                            std::shared_ptr<Connection> snapshot = afterRead->second.m_connection;
                            const int error = Internal::GetSocketPendingError(afterRead->second.m_fd);
                            ++m_impl->m_completedOperations;
                            ++batchItems;
                            PollerAccess::Error(*snapshot, error == 0 ? EIO : error);
                            continue;
                        }
                        if (HasReadinessEvent(event.events, IOEvent::Write)) {
                            batchItems += afterRead->second.m_datagram
                                ? DrainDatagramWrite(
                                    registration.m_connectionKey,
                                    registration.m_id)
                                : DrainConnectionWrite(
                                    registration.m_connectionKey,
                                    registration.m_id);
                        }
                    }
                }

                // 统一派发已到期 timer，并先删除 active 状态避免重复调用。
                while (true) {
                    m_impl->PruneTimeoutHeap();
                    if (m_impl->m_timeoutHeap.empty()
                        || m_impl->m_timeoutHeap.top().m_deadline
                            > std::chrono::steady_clock::now()) {
                        break;
                    }
                    const ReadinessTimeoutHeapNode node = m_impl->m_timeoutHeap.top(); // generation 快照
                    m_impl->m_timeoutHeap.pop();
                    const auto found = m_impl->m_timeouts.find(node.m_id);
                    if (found == m_impl->m_timeouts.end()
                        || found->second.m_deadline != node.m_deadline) {
                        continue;
                    }
                    TimeoutCallback callback = std::move(found->second.m_callback); // 先撤销 active
                    m_impl->m_timeouts.erase(found);
                    ++m_impl->m_completedOperations;
                    ++batchItems;
                    if (callback) callback();
                }
                m_impl->RecordCompletionBatch(batchItems);
            }

            void ReadinessCompletionPoller::Flush() {
                // 立即 connect 结果在 issuer 线程延迟到 Flush，允许调用方先拿到 operation id。
                if (m_impl == nullptr || !m_impl->m_activated) return;
                while (true) {
                    ConnectId readyId = InvalidConnectId; // 每次 callback 后重新查表
                    int readyError = 0; // 当前立即 connect 的系统结果
                    for (const auto& item : m_impl->m_connectStates) {
                        if (!item.second.m_readyToComplete) continue;
                        readyId = item.first;
                        readyError = item.second.m_error;
                        break;
                    }
                    if (readyId == InvalidConnectId) break;
                    CompleteConnect(readyId, readyError);
                }

                // 新写先尝试同步 drain，EAGAIN 后由 driver 写 readiness 继续推进。
                while (!m_impl->m_writeFlushQueue.empty()) {
                    const ReadinessWriteFlushItem item = m_impl->m_writeFlushQueue.front();
                    m_impl->m_writeFlushQueue.pop_front();
                    const auto found = m_impl->m_connectionStates.find(item.m_connectionKey);
                    if (found == m_impl->m_connectionStates.end()
                        || found->second.m_registrationId != item.m_registrationId) {
                        continue;
                    }
                    found->second.m_writeFlushQueued = false;
                    if (found->second.m_datagram) {
                        (void)DrainDatagramWrite(item.m_connectionKey, item.m_registrationId);
                    }
                    else {
                        (void)DrainConnectionWrite(item.m_connectionKey, item.m_registrationId);
                    }
                }
            }

            Poller::ConnectId ReadinessCompletionPoller::StartConnect(
                const Address& remoteAddress,
                ConnectCallback callback) noexcept {
                // 未激活、无效地址或空 callback 不创建无法回收的 socket。
                if (m_impl == nullptr || !m_impl->m_activated
                    || !remoteAddress.IsValid() || !callback) {
                    return InvalidConnectId;
                }

                const SocketType fd = Internal::CreateSocket(
                    remoteAddress.FamilyValue(),
                    SOCK_STREAM | SOCK_CLOEXEC,
                    IPPROTO_TCP); // completion 前由 connect state 独占
                if (fd == kInvalidSocket) {
                    SetLastError(Internal::GetLastSocketError());
                    return InvalidConnectId;
                }
                if (!Internal::SetNonBlocking(fd, true) || !Internal::SetTcpNoDelay(fd)) {
                    SetLastError(Internal::GetLastSocketError());
                    Internal::CloseSocket(fd);
                    return InvalidConnectId;
                }

                const ConnectId connectId = m_impl->NextConnectId(); // 调用方取消使用的 id
                ReadinessConnectState state{}; // state 持有 socket 与 callback 所有权
                state.m_id = connectId;
                state.m_fd = fd;
                state.m_callback = std::move(callback);
                try {
                    m_impl->m_connectStates.emplace(connectId, std::move(state));
                }
                catch (...) {
                    SetLastError(ENOMEM);
                    Internal::CloseSocket(fd);
                    return InvalidConnectId;
                }

                auto stored = m_impl->m_connectStates.find(connectId); // map 中地址稳定到完成
                const int connectResult = Internal::ConnectSocket(
                    fd,
                    remoteAddress.SockAddr(),
                    remoteAddress.Length());
                if (connectResult == 0) {
                    stored->second.m_readyToComplete = true;
                    stored->second.m_error = 0;
                    return connectId;
                }

                const int error = Internal::GetLastSocketError(); // 非阻塞 connect 初始结果
                if (error == EINPROGRESS) {
                    const ReadinessRegistrationId registrationId =
                        m_impl->NextRegistrationId(); // fd 复用不复用身份
                    ReadinessRegistration registration{};
                    registration.m_id = registrationId;
                    registration.m_kind = ReadinessRegistrationKind::Connect;
                    registration.m_fd = fd;
                    registration.m_operationId = connectId;
                    try {
                        m_impl->m_registrations.emplace(registrationId, registration);
                    }
                    catch (...) {
                        m_impl->m_connectStates.erase(connectId);
                        Internal::CloseSocket(fd);
                        SetLastError(ENOMEM);
                        return InvalidConnectId;
                    }
                    if (!m_impl->m_driver->Add(
                        registrationId,
                        fd,
                        IOEvent::Write | IOEvent::Error | IOEvent::Close)) {
                        const int registrationError = m_impl->m_driver->LastError();
                        m_impl->m_registrations.erase(registrationId);
                        m_impl->m_connectStates.erase(connectId);
                        Internal::CloseSocket(fd);
                        SetLastError(registrationError);
                        return InvalidConnectId;
                    }
                    stored->second.m_registrationId = registrationId;
                }
                else {
                    stored->second.m_readyToComplete = true; // 同步失败也延迟到 Flush
                    stored->second.m_error = error;
                }
                return connectId;
            }

            void ReadinessCompletionPoller::CancelConnect(ConnectId connectId) noexcept {
                // 取消先撤销 registration，再关闭仍由 common Poller 独占的 socket。
                if (m_impl == nullptr || connectId == InvalidConnectId) return;
                const auto found = m_impl->m_connectStates.find(connectId);
                if (found == m_impl->m_connectStates.end()) return;
                const ReadinessRegistrationId registrationId = found->second.m_registrationId;
                const SocketType fd = found->second.m_fd;
                RemoveRegistration(registrationId);
                m_impl->m_connectStates.erase(found);
                Internal::CloseSocket(fd);
            }

            bool ReadinessCompletionPoller::StartConnection(
                const std::shared_ptr<Connection>& connection) {
                // built-in TCP/UDP 共用同一 readiness registration 与关闭状态。
                if (m_impl == nullptr || !m_impl->m_activated || !connection
                    || connection->GetSocket() == kInvalidSocket
                    || (connection->GetTransportKind() != TransportKind::Tcp
                        && connection->GetTransportKind() != TransportKind::Udp)) {
                    return false;
                }
                if (m_impl->m_connectionStates.find(connection.get())
                    != m_impl->m_connectionStates.end()) {
                    return true;
                }

                const ReadinessRegistrationId registrationId = m_impl->NextRegistrationId();
                ReadinessRegistration registration{};
                registration.m_id = registrationId;
                registration.m_kind = ReadinessRegistrationKind::Connection;
                registration.m_fd = connection->GetSocket();
                registration.m_connectionKey = connection.get();
                try {
                    ReadinessConnectionState state{}; // map value持有 Connection 与 BufferChain
                    state.m_connection = connection;
                    state.m_registrationId = registrationId;
                    state.m_fd = connection->GetSocket();
                    state.m_datagram = connection->GetTransportKind() == TransportKind::Udp;
                    state.m_readEnabled = PollerAccess::ReadEnabled(*connection);
                    state.m_maxDatagramBytes = state.m_datagram
                        ? PollerAccess::MaxDatagramBytes(*connection)
                        : 0;
                    m_impl->m_connectionStates.emplace(connection.get(), std::move(state));
                    m_impl->m_registrations.emplace(registrationId, registration);
                }
                catch (...) {
                    m_impl->m_connectionStates.erase(connection.get());
                    m_impl->m_registrations.erase(registrationId);
                    SetLastError(ENOMEM);
                    return false;
                }

                const auto found = m_impl->m_connectionStates.find(connection.get());
                IOEvent events = IOEvent::Error | IOEvent::Close; // TCP 始终观察关闭与错误
                if (found->second.m_readEnabled) events |= IOEvent::Read;
                if (!m_impl->m_driver->Add(
                    registrationId,
                    connection->GetSocket(),
                    events)) {
                    const int error = m_impl->m_driver->LastError();
                    m_impl->m_connectionStates.erase(connection.get());
                    m_impl->m_registrations.erase(registrationId);
                    SetLastError(error);
                    return false;
                }
                if (found->second.m_readEnabled) ++m_impl->m_readSubmissions;
                return true;
            }

            bool ReadinessCompletionPoller::StartAccept(
                SocketType listenFd,
                AcceptCallback callback) {
                // listener 由调用方拥有，common Poller 只维护 readiness registration。
                if (m_impl == nullptr || !m_impl->m_activated
                    || listenFd == kInvalidSocket || !callback) {
                    return false;
                }
                if (m_impl->m_acceptStates.find(listenFd) != m_impl->m_acceptStates.end()) {
                    return true;
                }

                const ReadinessRegistrationId registrationId = m_impl->NextRegistrationId();
                ReadinessAcceptState state{}; // callback 只在 issuer 线程执行
                state.m_fd = listenFd;
                state.m_registrationId = registrationId;
                state.m_callback = std::move(callback);
                ReadinessRegistration registration{};
                registration.m_id = registrationId;
                registration.m_kind = ReadinessRegistrationKind::Accept;
                registration.m_fd = listenFd;
                try {
                    m_impl->m_acceptStates.emplace(listenFd, std::move(state));
                    m_impl->m_registrations.emplace(registrationId, registration);
                }
                catch (...) {
                    m_impl->m_acceptStates.erase(listenFd);
                    m_impl->m_registrations.erase(registrationId);
                    SetLastError(ENOMEM);
                    return false;
                }
                if (!m_impl->m_driver->Add(
                    registrationId,
                    listenFd,
                    IOEvent::Read | IOEvent::Error | IOEvent::Close)) {
                    const int error = m_impl->m_driver->LastError();
                    m_impl->m_acceptStates.erase(listenFd);
                    m_impl->m_registrations.erase(registrationId);
                    SetLastError(error);
                    return false;
                }
                return true;
            }

            void ReadinessCompletionPoller::StopAccept(SocketType listenFd) {
                // 停止先撤销 registration，随后移除 listener callback 状态。
                if (m_impl == nullptr || listenFd == kInvalidSocket) return;
                const auto found = m_impl->m_acceptStates.find(listenFd);
                if (found == m_impl->m_acceptStates.end()) return;
                const ReadinessRegistrationId registrationId = found->second.m_registrationId;
                RemoveRegistration(registrationId);
                m_impl->m_acceptStates.erase(found);
            }

            Poller::TimeoutId ReadinessCompletionPoller::ScheduleTimeout(
                std::chrono::milliseconds delay,
                TimeoutCallback callback) noexcept {
                // 未激活或空回调不创建不可回收的 timer generation。
                if (m_impl == nullptr || !m_impl->m_activated || !callback) {
                    return InvalidTimeoutId;
                }

                    const Poller::TimeoutId timeoutId = m_impl->NextTimeoutId(); // issuer 独占分配 id
                ReadinessTimeoutState state{}; // active 表持有 callback 所有权
                state.m_id = timeoutId;
                state.m_deadline = std::chrono::steady_clock::now() + delay;
                state.m_callback = std::move(callback);
                try {
                    const auto inserted = m_impl->m_timeouts.emplace(timeoutId, std::move(state));
                    if (!inserted.second) return InvalidTimeoutId;
                    m_impl->m_timeoutHeap.push({ inserted.first->second.m_deadline, timeoutId });
                }
                catch (...) {
                    m_impl->m_timeouts.erase(timeoutId);
                    return InvalidTimeoutId;
                }
                return timeoutId;
            }

            void ReadinessCompletionPoller::CancelTimeout(Poller::TimeoutId timeoutId) noexcept {
                // active 状态删除后，旧 heap 节点由下一轮 prune 惰性回收。
                if (m_impl == nullptr || timeoutId == InvalidTimeoutId) return;
                m_impl->m_timeouts.erase(timeoutId);
            }

            bool ReadinessCompletionPoller::QueueWrite(
                Connection* connection,
                Buffer&& buffer) {
                // 单 Buffer 入口统一进入多段所有权链。
                BufferChain chain;
                chain.Append(std::move(buffer));
                return QueueWrite(connection, std::move(chain));
            }

            bool ReadinessCompletionPoller::QueueWrite(
                Connection* connection,
                BufferChain&& chain) {
                // 只移动 Buffer PImpl，并在写完成前保持全部段所有权。
                if (m_impl == nullptr || connection == nullptr) return false;
                const auto found = m_impl->m_connectionStates.find(connection);
                if (found == m_impl->m_connectionStates.end()) return false;
                const std::size_t incomingBytes = chain.ReadableBytes(); // 移动前统计
                if (found->second.m_closing || incomingBytes == 0) return true;
                if (incomingBytes > (std::numeric_limits<std::size_t>::max)()
                    - found->second.m_pendingWriteBytes) {
                    std::shared_ptr<Connection> snapshot = found->second.m_connection;
                    PollerAccess::Error(*snapshot, ENOBUFS);
                    return true;
                }
                try {
                    found->second.m_writeChain.Append(std::move(chain));
                }
                catch (...) {
                    std::shared_ptr<Connection> snapshot = found->second.m_connection;
                    PollerAccess::Error(*snapshot, ENOMEM);
                    return true;
                }
                found->second.m_pendingWriteBytes += incomingBytes;
                const std::size_t pendingBytes = found->second.m_pendingWriteBytes; // callback 快照
                const ReadinessRegistrationId registrationId = found->second.m_registrationId;
                std::shared_ptr<Connection> snapshot = found->second.m_connection; // callback 保活
                if (!PollerAccess::WriteGrowth(*snapshot, pendingBytes)) return true;
                const auto current = m_impl->m_connectionStates.find(connection);
                if (current == m_impl->m_connectionStates.end()
                    || current->second.m_registrationId != registrationId
                    || current->second.m_closing) {
                    return true;
                }
                if (!current->second.m_writeFlushQueued) {
                    try {
                        m_impl->m_writeFlushQueue.push_back({ connection, registrationId });
                        current->second.m_writeFlushQueued = true;
                    }
                    catch (...) {
                        PollerAccess::Error(*snapshot, ENOMEM);
                    }
                }
                return true;
            }

            bool ReadinessCompletionPoller::QueueDatagramWrite(
                Connection* connection,
                const Address& peer,
                Buffer&& buffer) {
                // 数据报节点保持边界、peer 与 queue cost，完成前不拆分 payload。
                if (m_impl == nullptr || connection == nullptr) return false;
                const auto found = m_impl->m_connectionStates.find(connection);
                if (found == m_impl->m_connectionStates.end() || !found->second.m_datagram) {
                    return false;
                }
                if (found->second.m_closing) return true;
                const DatagramWriteTarget target = ResolveDatagramWriteTarget(
                    connection->GetRemoteAddress().IsValid(),
                    peer.IsValid());
                if (target == DatagramWriteTarget::Invalid) return false;
                const std::size_t queueCost = DatagramQueueCost(buffer.ReadableBytes());
                if (queueCost > (std::numeric_limits<std::size_t>::max)()
                    - found->second.m_pendingWriteBytes) {
                    std::shared_ptr<Connection> snapshot = found->second.m_connection;
                    PollerAccess::Error(*snapshot, ENOBUFS);
                    return true;
                }
                try {
                    found->second.m_datagramWrites.emplace_back(
                        target,
                        peer,
                        std::move(buffer),
                        queueCost);
                }
                catch (...) {
                    std::shared_ptr<Connection> snapshot = found->second.m_connection;
                    PollerAccess::Error(*snapshot, ENOMEM);
                    return true;
                }
                found->second.m_pendingWriteBytes += queueCost;
                ++m_impl->m_pendingDatagramSends;
                const std::size_t pendingBytes = found->second.m_pendingWriteBytes;
                const ReadinessRegistrationId registrationId = found->second.m_registrationId;
                std::shared_ptr<Connection> snapshot = found->second.m_connection;
                if (!PollerAccess::WriteGrowth(*snapshot, pendingBytes)) return true;
                const auto current = m_impl->m_connectionStates.find(connection);
                if (current == m_impl->m_connectionStates.end()
                    || current->second.m_registrationId != registrationId
                    || current->second.m_closing) {
                    return true;
                }
                if (!current->second.m_writeFlushQueued) {
                    try {
                        m_impl->m_writeFlushQueue.push_back({ connection, registrationId });
                        current->second.m_writeFlushQueued = true;
                    }
                    catch (...) {
                        PollerAccess::Error(*snapshot, ENOMEM);
                    }
                }
                return true;
            }

            void ReadinessCompletionPoller::SetReadEnabled(
                Connection* connection,
                bool enabled) {
                // 读开关只更新同一 registration 的 interest，不制造身份切换。
                if (m_impl == nullptr || connection == nullptr) return;
                const auto found = m_impl->m_connectionStates.find(connection);
                if (found == m_impl->m_connectionStates.end() || found->second.m_closing
                    || found->second.m_readEnabled == enabled) {
                    return;
                }
                found->second.m_readEnabled = enabled;
                if (enabled) ++m_impl->m_readSubmissions;
                std::shared_ptr<Connection> snapshot = found->second.m_connection;
                if (!UpdateConnectionInterest(connection)) PollerAccess::Error(*snapshot, EIO);
            }

            std::size_t ReadinessCompletionPoller::PendingWriteBytes(
                const Connection* connection) const noexcept {
                // 查询只返回 issuer 状态快照，不转移 BufferChain 所有权。
                if (m_impl == nullptr || connection == nullptr) return 0;
                const auto found = m_impl->m_connectionStates.find(
                    const_cast<Connection*>(connection));
                return found == m_impl->m_connectionStates.end()
                    ? 0
                    : found->second.m_pendingWriteBytes;
            }

            void ReadinessCompletionPoller::StopConnection(Connection* connection) {
                // 撤销 registration 并同步释放全部待写所有权。
                if (m_impl == nullptr || connection == nullptr) return;
                const auto found = m_impl->m_connectionStates.find(connection);
                if (found == m_impl->m_connectionStates.end()) return;
                found->second.m_closing = true;
                found->second.m_readEnabled = false;
                const ReadinessRegistrationId registrationId = found->second.m_registrationId;
                RemoveRegistration(registrationId);
                if (found->second.m_datagram) {
                    std::shared_ptr<Connection> snapshot = found->second.m_connection;
                    std::deque<ReadinessDatagramWriteState> abandoned;
                    abandoned.swap(found->second.m_datagramWrites);
                    const std::size_t abandonedCount = abandoned.size();
                    found->second.m_pendingWriteBytes = 0;
                    m_impl->m_pendingDatagramSends = abandonedCount >= m_impl->m_pendingDatagramSends
                        ? 0
                        : m_impl->m_pendingDatagramSends - abandonedCount;
                    m_impl->m_connectionStates.erase(found);
                    for (const ReadinessDatagramWriteState& datagram : abandoned) {
                        PollerAccess::DatagramWriteCompleted(
                            *snapshot,
                            datagram.m_peer,
                            datagram.m_queueCost);
                    }
                    return;
                }
                found->second.m_writeChain.Clear();
                found->second.m_pendingWriteBytes = 0;
                m_impl->m_connectionStates.erase(found);
            }

            bool ReadinessCompletionPoller::HasPendingShutdownCompletions() const noexcept {
                // readiness shell 没有异步 terminal completion。
                return false;
            }

            std::uint64_t ReadinessCompletionPoller::CompletedOperationCount() const noexcept {
                // 返回 common core 已分发的 completion 总数。
                return m_impl != nullptr ? m_impl->m_completedOperations : 0;
            }

            std::size_t ReadinessCompletionPoller::ProvidedBufferCount() const noexcept {
                // readiness backend 不提供 io_uring buffer。
                return 0;
            }

            std::uint64_t ReadinessCompletionPoller::ReadSubmissionCount() const noexcept {
                // 返回启用 TCP read interest 的累计次数。
                return m_impl != nullptr ? m_impl->m_readSubmissions : 0;
            }

            std::uint64_t ReadinessCompletionPoller::AcceptSubmissionCount() const noexcept {
                // shell 尚未提交 accept。
                return 0;
            }

            bool ReadinessCompletionPoller::ReceiveBundleEnabled() const noexcept {
                // readiness backend 不模拟 receive bundle。
                return false;
            }

            CompletionStats ReadinessCompletionPoller::GetCompletionStats() const noexcept {
                // readiness 不模拟 io_uring capability，只填充共用 completion 计数。
                CompletionStats stats{};
                if (m_impl == nullptr) return stats;
                stats.completedOperations = m_impl->m_completedOperations;
                stats.completionBatchCount = m_impl->m_completionBatchCount;
                stats.completionBatchItems = m_impl->m_completionBatchItems;
                stats.peakCompletionBatch = m_impl->m_peakCompletionBatch;
                stats.receivedBytes = m_impl->m_receivedBytes;
                stats.receiveCompletions = m_impl->m_receiveCompletions;
                stats.datagramReceiveCompletions = m_impl->m_datagramReceiveCompletions;
                stats.datagramSendCompletions = m_impl->m_datagramSendCompletions;
                stats.receivedDatagrams = m_impl->m_receivedDatagrams;
                stats.sentDatagrams = m_impl->m_sentDatagrams;
                stats.zeroLengthDatagrams = m_impl->m_zeroLengthDatagrams;
                stats.truncatedDatagrams = m_impl->m_truncatedDatagrams;
                stats.pendingDatagramSends = m_impl->m_pendingDatagramSends;
                stats.datagramSendBatchSubmissions = m_impl->m_datagramSendBatchSubmissions;
                stats.maximumDatagramSendBatch = m_impl->m_maximumDatagramSendBatch;
                return stats;
            }

            const char* ReadinessCompletionPoller::BackendName() const noexcept {
                // driver 激活前也保持稳定诊断名。
                return m_impl != nullptr && m_impl->m_driver != nullptr
                    ? m_impl->m_driver->BackendName()
                    : "readiness-uninitialized";
            }
        }
    }
}
