#include "net/platform/windows/IocpPoller.hpp"
#include "net/platform/windows/IocpOperation.hpp"
#include "net/platform/SocketOps.hpp"
#include "net/DatagramCompletionPolicy.hpp"
#include "net/PollerAccess.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <LikesProgram/Net/Address.hpp>
#include <LikesProgram/Net/BufferChain.hpp>
#include <LikesProgram/Net/Connection.hpp>
#include <winsock2.h>
#include <windows.h>
#include <mswsock.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <cerrno>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <queue>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {
    constexpr std::size_t kIocpUdpWireCapacity = 65535; // 标准 UDP 最大 payload

    using LikesProgram::Net::Poller;
    using LikesProgram::Net::SocketType;
    using LikesProgram::Net::kInvalidSocket;
    using LikesProgram::Net::Internal::IocpOperation;

    struct IocpTimeoutState {
        std::chrono::steady_clock::time_point m_deadline{};
        Poller::TimeoutCallback m_callback;
    };

    struct IocpTimeoutNode {
        std::chrono::steady_clock::time_point m_deadline{};
        Poller::TimeoutId m_id = Poller::InvalidTimeoutId;
    };

    struct IocpTimeoutCompare {
        bool operator()(const IocpTimeoutNode& left, const IocpTimeoutNode& right) const noexcept {
            return left.m_deadline > right.m_deadline;
        }
    };

    [[maybe_unused]] ULONG_PTR EncodeSocketKey(std::uint64_t generation) {
        if (generation > (std::numeric_limits<ULONG_PTR>::max)() >> 1) {
            throw std::overflow_error("IOCP socket generation exhausted");
        }
        return static_cast<ULONG_PTR>(generation << 1);
    }

    ULONG_PTR EncodeChannelKey(std::uint64_t registrationId) {
        if (registrationId > (std::numeric_limits<ULONG_PTR>::max)() >> 1) {
            throw std::overflow_error("IOCP Channel registration exhausted");
        }
        return static_cast<ULONG_PTR>((registrationId << 1) | 1);
    }

    long ToWindowsNetworkMask(LikesProgram::Net::IOEvent events) noexcept {
        long mask = 0;
        if (LikesProgram::Net::HasEvent(events, LikesProgram::Net::IOEvent::Read)) {
            mask |= FD_READ;
        }
        if (LikesProgram::Net::HasEvent(events, LikesProgram::Net::IOEvent::Write)) {
            mask |= FD_WRITE;
        }
        if (LikesProgram::Net::HasEvent(events, LikesProgram::Net::IOEvent::Close)) {
            mask |= FD_CLOSE;
        }
        return mask;
    }

    DWORD WaitMillisecondsUntil(
        std::chrono::steady_clock::time_point deadline) noexcept {
        const auto now = std::chrono::steady_clock::now();
        if (deadline <= now) return 0;

        const auto remaining = deadline - now;
        const auto rounded = std::chrono::ceil<std::chrono::milliseconds>(remaining);
        if (rounded.count() >= static_cast<long long>(INFINITE - 1)) {
            return INFINITE - 1;
        }
        return static_cast<DWORD>(rounded.count());
    }
}

namespace LikesProgram {
    namespace Net {
        namespace Internal {
            struct IocpChannelState {
                std::uint64_t m_registrationId = 0;
                Channel* m_channel = nullptr;
                SocketType m_fd = kInvalidSocket;
                WSAEVENT m_event = WSA_INVALID_EVENT;
                PTP_WAIT m_wait = nullptr;
                long m_networkMask = 0;
                HANDLE m_completionPort = nullptr;
            };

            struct IocpConnectOperation final : IocpOperation {
                using ConnectId = Poller::ConnectId;
                using ConnectCallback = Poller::ConnectCallback;

                IocpConnectOperation(
                    ConnectId connectId,
                    SocketType fd,
                    const Address& remoteAddress,
                    ConnectCallback callback)
                    : m_connectId(connectId),
                    m_remoteAddress(remoteAddress),
                    m_callback(std::move(callback)) {
                    m_kind = IocpOperationKind::Connect;
                    m_id = connectId;
                    m_fd = fd;
                }

                ConnectId m_connectId = Poller::InvalidConnectId;
                Address m_remoteAddress;
                ConnectCallback m_callback;
            };

            struct IocpAcceptOperation final : IocpOperation {
                using AcceptCallback = Poller::AcceptCallback;

                // 固定 listener、accepted socket 与回调到 OVERLAPPED terminal packet。
                IocpAcceptOperation(
                    SocketType listenFd,
                    SocketType acceptedFd,
                    AcceptCallback callback)
                    : m_listenFd(listenFd),
                    m_acceptedFd(acceptedFd),
                    m_callback(std::move(callback)) {
                    m_kind = IocpOperationKind::Accept;
                    m_fd = listenFd;
                }

                SocketType m_listenFd = kInvalidSocket; // 提交 AcceptEx 的 listener
                SocketType m_acceptedFd = kInvalidSocket; // completion 前由 operation 独占
                AcceptCallback m_callback; // 成功 terminal packet 的单次用户回调
                std::array<std::uint8_t,
                    2 * (sizeof(sockaddr_storage) + 16)> m_addressBuffer{}; // AcceptEx 双地址缓冲
            };

            struct IocpAcceptState {
                SocketType m_listenFd = kInvalidSocket; // listener 状态表键值
                IocpAcceptOperation* m_operation = nullptr; // 当前唯一 outstanding operation
                Poller::AcceptCallback m_callback; // replacement operation 的回调模板
                bool m_closing = false; // StopAccept 后拒绝 replacement 与用户回调
            };

            struct IocpConnectionState;

            struct IocpDatagramWriteState {
                IocpDatagramWriteState()
                    : m_buffer(0) {
                }

                Address m_peer; // 显式 peer，connected 发送时保持为空地址
                Buffer m_buffer; // datagram completion 前独占完整 payload
                std::size_t m_queueCost = 0; // 空包也占一个背压单位
                bool m_connected = false; // true 时使用 WSASend
            };

            struct IocpTcpReadOperation final : IocpOperation {
                explicit IocpTcpReadOperation(IocpConnectionState* state, SocketType fd)
                    : m_state(state), m_buffer(64 * 1024) {
                    m_kind = IocpOperationKind::TcpRead;
                    m_fd = fd;
                }

                IocpConnectionState* m_state = nullptr; // terminal packet 回查连接状态
                Buffer m_buffer; // overlapped read 完成前独占接收存储
                WSABUF m_bufferView{}; // WSARecv 使用的稳定视图
            };

            struct IocpTcpWriteOperation final : IocpOperation {
                explicit IocpTcpWriteOperation(IocpConnectionState* state, SocketType fd)
                    : m_state(state) {
                    m_kind = IocpOperationKind::TcpWrite;
                    m_fd = fd;
                }

                IocpConnectionState* m_state = nullptr; // terminal packet 回查连接状态
                WSABUF m_bufferView{}; // WSASend 当前链段视图
                std::size_t m_submittedBytes = 0; // 当前 completion 最多可消费的链段长度
            };

            struct IocpUdpReadOperation final : IocpOperation {
                explicit IocpUdpReadOperation(IocpConnectionState* state, SocketType fd)
                    : m_state(state), m_buffer(64 * 1024) {
                    m_kind = IocpOperationKind::UdpRead;
                    m_fd = fd;
                }

                IocpConnectionState* m_state = nullptr; // terminal packet 回查连接状态
                Buffer m_buffer; // 固定 65535-byte wire buffer
                WSABUF m_bufferView{}; // WSARecvFrom 使用的稳定视图
                sockaddr_storage m_peer{}; // 本次数据报发送方地址
                SocketLength m_peerLength = static_cast<SocketLength>(sizeof(m_peer)); // 地址长度槽
                DWORD m_flags = 0; // Winsock 数据报 flags 输出槽
            };

            struct IocpUdpWriteOperation final : IocpOperation {
                explicit IocpUdpWriteOperation(
                    IocpConnectionState* state,
                    SocketType fd,
                    IocpDatagramWriteState* datagram)
                    : m_state(state), m_datagram(datagram) {
                    m_kind = IocpOperationKind::UdpWrite;
                    m_fd = fd;
                }

                IocpConnectionState* m_state = nullptr; // terminal packet 回查连接状态
                IocpDatagramWriteState* m_datagram = nullptr; // FIFO 节点稳定地址
                WSABUF m_bufferView{}; // WSASend/WSASendTo 当前数据报视图
            };

            struct IocpConnectionState {
                std::shared_ptr<Connection> m_connection; // completion 期间保持 Connection 存活
                SocketType m_fd = kInvalidSocket; // overlapped operation 使用的稳定 fd 快照
                ULONG_PTR m_completionKey = 0; // socket association 的 generation key
                bool m_datagram = false; // UDP 使用数据报 operation，TCP 使用字节流 operation
                std::size_t m_maxDatagramBytes = 0; // 业务可见的单包容量上限
                bool m_closing = false; // 关闭后只回收 terminal packet
                bool m_readEnabled = true; // 业务背压读开关
                bool m_readOutstanding = false; // 当前是否持有 WSARecv operation
                bool m_writeOutstanding = false; // 当前是否持有 WSASend operation
                bool m_readCancelRequested = false; // 防止重复取消 read
                bool m_writeCancelRequested = false; // 防止重复取消 write
                std::size_t m_pendingWriteBytes = 0; // 写链未完成总字节数
                BufferChain m_writeChain; // TCP partial send 前保持全部段所有权
                std::deque<IocpDatagramWriteState> m_datagramWrites; // UDP peer/FIFO 所有权队列
                IocpOperation* m_readOperation = nullptr; // map 内稳定 operation 地址
                IocpOperation* m_writeOperation = nullptr; // map 内稳定 operation 地址
            };
        }
    }
}

namespace {
    VOID CALLBACK ChannelWaitCallback(
        PTP_CALLBACK_INSTANCE,
        PVOID context,
        PTP_WAIT,
        TP_WAIT_RESULT) noexcept {
        auto* state = static_cast<LikesProgram::Net::Internal::IocpChannelState*>(context);
        if (state == nullptr || state->m_completionPort == nullptr) return;
        (void)::PostQueuedCompletionStatus(
            state->m_completionPort,
            0,
            EncodeChannelKey(state->m_registrationId),
            nullptr);
    }
}

namespace LikesProgram {
    namespace Net {
        namespace Internal {
            struct IocpPollerImpl {
                using TimeoutId = Poller::TimeoutId;

                HANDLE m_completionPort = nullptr;
                bool m_activated = false;
                std::uint64_t m_nextOperationId = 1;
                std::uint64_t m_nextGeneration = 1;
                std::uint64_t m_nextTimeoutId = 1;
                std::uint64_t m_nextRegistrationId = 1;
                Poller::ConnectId m_nextConnectId = 1;
                LPFN_CONNECTEX m_connectEx = nullptr;
                bool m_connectExLoaded = false;
                int m_connectExError = 0;
                LPFN_ACCEPTEX m_acceptEx = nullptr; // 当前 Winsock provider 的 AcceptEx 入口
                int m_acceptExError = 0; // 最近一次 AcceptEx 加载失败码
                std::unordered_map<OVERLAPPED*, std::unique_ptr<IocpOperation>> m_operations;
                std::array<OVERLAPPED_ENTRY, 256> m_entries{};
                std::unordered_map<
                    std::uint64_t,
                    std::unique_ptr<IocpChannelState>> m_channelStates;
                std::unordered_map<Channel*, std::uint64_t> m_channelRegistrationIds;
                std::unordered_map<TimeoutId, IocpTimeoutState> m_timeouts;
                std::priority_queue<
                    IocpTimeoutNode,
                    std::vector<IocpTimeoutNode>,
                    IocpTimeoutCompare> m_timeoutHeap;
                std::uint64_t m_completedOperations = 0;
                std::uint64_t m_completionBatchCount = 0;
                std::uint64_t m_completionBatchItems = 0;
                std::uint64_t m_peakCompletionBatch = 0;
                std::uint64_t m_unknownPackets = 0;
                std::uint64_t m_connectSubmissions = 0;
                std::uint64_t m_connectCancelSubmissions = 0;
                std::uint64_t m_acceptSubmissions = 0; // 成功进入 IOCP 的 accept 提交数
                std::unordered_map<
                    Poller::ConnectId,
                    IocpConnectOperation*> m_connectStates;
                std::unordered_map<SocketType, ULONG_PTR> m_preAssociatedSockets; // ConnectEx 成功后转交 Connection 的既有关联
                std::unordered_map<SocketType, IocpAcceptState> m_acceptStates; // listener 单 issuer 状态
                std::unordered_map<
                    Connection*,
                    std::unique_ptr<IocpConnectionState>> m_connectionStates; // TCP owning operation 状态
                std::uint64_t m_readSubmissions = 0; // 已提交 WSARecv operation 数
                std::uint64_t m_datagramReceiveCompletions = 0; // 成功 UDP recvfrom completion 数
                std::uint64_t m_datagramSendCompletions = 0; // 成功 UDP send/sendto completion 数
                std::uint64_t m_receivedDatagrams = 0; // 已交付业务的数据报数
                std::uint64_t m_sentDatagrams = 0; // 已完成发送的数据报数
                std::uint64_t m_zeroLengthDatagrams = 0; // 收发零长度数据报总数
                std::uint64_t m_truncatedDatagrams = 0; // 按业务容量截断的数据报数
                std::size_t m_pendingDatagramSends = 0; // 当前等待 completion 的 datagram 节点数

                std::uint64_t NextRegistrationId() noexcept {
                    if (m_nextRegistrationId == 0) return 0;
                    const std::uint64_t id = m_nextRegistrationId++;
                    if (m_nextRegistrationId == 0) m_nextRegistrationId = 1;
                    return id;
                }

                Poller::ConnectId NextConnectId() noexcept {
                    Poller::ConnectId id = m_nextConnectId++;
                    while (id == Poller::InvalidConnectId
                        || m_connectStates.find(id) != m_connectStates.end()) {
                        id = m_nextConnectId++;
                    }
                    return id;
                }

                void PruneTimeoutHeap() noexcept {
                    while (!m_timeoutHeap.empty()) {
                        const IocpTimeoutNode& node = m_timeoutHeap.top();
                        const auto found = m_timeouts.find(node.m_id);
                        if (found != m_timeouts.end()
                            && found->second.m_deadline == node.m_deadline) {
                            return;
                        }
                        m_timeoutHeap.pop();
                    }
                }

                TimeoutId NextTimeoutId() noexcept {
                    if (m_nextTimeoutId == Poller::InvalidTimeoutId) {
                        return Poller::InvalidTimeoutId;
                    }
                    const TimeoutId id = m_nextTimeoutId++;
                    if (m_nextTimeoutId == Poller::InvalidTimeoutId) m_nextTimeoutId = 1;
                    return id;
                }

                void RecordBatch(std::uint64_t count) noexcept {
                    if (count == 0) return;
                    ++m_completionBatchCount;
                    m_completionBatchItems += count;
                    m_peakCompletionBatch = (std::max)(m_peakCompletionBatch, count);
                }

                // 为连接提交一个稳定地址的 overlapped read。
                bool SubmitTcpRead(IocpConnectionState& state) noexcept;
                // 为连接提交当前写链首段的 overlapped write。
                bool SubmitTcpWrite(IocpConnectionState& state) noexcept;
                // 为 UDP 连接提交一个固定 wire buffer 的 recvfrom operation。
                bool SubmitUdpRead(IocpConnectionState& state) noexcept;
                // 为 UDP FIFO 首节点提交一个完整 send/sendto operation。
                bool SubmitUdpWrite(IocpConnectionState& state) noexcept;
                // 消费一个 read terminal packet 并按状态机续投。
                void CompleteTcpRead(
                    IocpTcpReadOperation& operation,
                    const OVERLAPPED_ENTRY& entry) noexcept;
                // 消费一个 write terminal packet 并按 partial send 续投。
                void CompleteTcpWrite(
                    IocpTcpWriteOperation& operation,
                    const OVERLAPPED_ENTRY& entry) noexcept;
                // 消费 UDP recvfrom terminal packet 并交付 peer/truncation 元数据。
                void CompleteUdpRead(
                    IocpUdpReadOperation& operation,
                    const OVERLAPPED_ENTRY& entry) noexcept;
                // 消费 UDP send terminal packet 并按 FIFO 归还 queue cost。
                void CompleteUdpWrite(
                    IocpUdpWriteOperation& operation,
                    const OVERLAPPED_ENTRY& entry) noexcept;
                // 关闭时取消连接上的指定 operation。
                void CancelConnectionOperation(IocpOperation& operation) noexcept;
                // 关闭后仅在两个 terminal operation 都回收时移除状态。
                void CleanupClosedConnection(Connection* connection) noexcept;
            };

            bool IocpPollerImpl::SubmitTcpRead(IocpConnectionState& state) noexcept {
                if (!m_activated || state.m_closing || !state.m_readEnabled
                    || state.m_readOutstanding || state.m_fd == kInvalidSocket) {
                    return false;
                }

                std::unique_ptr<IocpTcpReadOperation> operation;
                IocpTcpReadOperation* raw = nullptr;
                try {
                    operation = std::make_unique<IocpTcpReadOperation>(
                        &state,
                        state.m_fd);
                    raw = operation.get();
                    raw->m_bufferView.buf = reinterpret_cast<char*>(
                        raw->m_buffer.PrepareWrite(64 * 1024));
                    raw->m_bufferView.len = 64 * 1024;
                    m_operations.emplace(&raw->m_overlapped, std::move(operation));
                    state.m_readOperation = raw;
                    state.m_readOutstanding = true;
                    state.m_readCancelRequested = false;
                }
                catch (...) {
                    if (raw != nullptr) m_operations.erase(&raw->m_overlapped);
                    state.m_readOperation = nullptr;
                    state.m_readOutstanding = false;
                    ::WSASetLastError(WSAENOBUFS);
                    return false;
                }

                DWORD flags = 0; // WSARecv 可能在同步完成时回写 flags
                DWORD received = 0; // overlapped 路径的同步输出槽
                if (::WSARecv(
                        state.m_fd,
                        &raw->m_bufferView,
                        1,
                        &received,
                        &flags,
                        &raw->m_overlapped,
                        nullptr) == SOCKET_ERROR) {
                    const int error = ::WSAGetLastError();
                    if (error != WSA_IO_PENDING) {
                        raw->m_syntheticCompletion = true;
                        raw->m_syntheticError = error;
                        if (!::PostQueuedCompletionStatus(
                                m_completionPort,
                                0,
                                state.m_completionKey,
                                &raw->m_overlapped)) {
                            m_operations.erase(&raw->m_overlapped);
                            state.m_readOperation = nullptr;
                            state.m_readOutstanding = false;
                            ::WSASetLastError(::GetLastError());
                            return false;
                        }
                    }
                }
                ++m_readSubmissions;
                return true;
            }

            bool IocpPollerImpl::SubmitTcpWrite(IocpConnectionState& state) noexcept {
                if (!m_activated || state.m_closing || state.m_writeOutstanding
                    || state.m_fd == kInvalidSocket || state.m_writeChain.Empty()) {
                    return false;
                }

                const BufferSlice segment = state.m_writeChain.Segment(0);
                if (segment.Empty()) return false;

                std::unique_ptr<IocpTcpWriteOperation> operation;
                IocpTcpWriteOperation* raw = nullptr;
                try {
                    operation = std::make_unique<IocpTcpWriteOperation>(&state, state.m_fd);
                    raw = operation.get();
                    raw->m_submittedBytes = (std::min)(
                        segment.Size(),
                        static_cast<std::size_t>((std::numeric_limits<ULONG>::max)()));
                    raw->m_bufferView.buf = reinterpret_cast<char*>(
                        const_cast<std::uint8_t*>(segment.Data()));
                    raw->m_bufferView.len = static_cast<ULONG>(raw->m_submittedBytes);
                    m_operations.emplace(&raw->m_overlapped, std::move(operation));
                    state.m_writeOperation = raw;
                    state.m_writeOutstanding = true;
                    state.m_writeCancelRequested = false;
                }
                catch (...) {
                    if (raw != nullptr) m_operations.erase(&raw->m_overlapped);
                    state.m_writeOperation = nullptr;
                    state.m_writeOutstanding = false;
                    ::WSASetLastError(WSAENOBUFS);
                    return false;
                }

                DWORD sent = 0; // WSASend 同步完成时的输出槽
                if (::WSASend(
                        state.m_fd,
                        &raw->m_bufferView,
                        1,
                        &sent,
                        0,
                        &raw->m_overlapped,
                        nullptr) == SOCKET_ERROR) {
                    const int error = ::WSAGetLastError();
                    if (error != WSA_IO_PENDING) {
                        raw->m_syntheticCompletion = true;
                        raw->m_syntheticError = error;
                        if (!::PostQueuedCompletionStatus(
                                m_completionPort,
                                0,
                                state.m_completionKey,
                                &raw->m_overlapped)) {
                            m_operations.erase(&raw->m_overlapped);
                            state.m_writeOperation = nullptr;
                            state.m_writeOutstanding = false;
                            ::WSASetLastError(::GetLastError());
                            return false;
                        }
                    }
                }
                return true;
            }

            bool IocpPollerImpl::SubmitUdpRead(IocpConnectionState& state) noexcept {
                if (!m_activated || state.m_closing || !state.m_readEnabled
                    || state.m_readOutstanding || state.m_fd == kInvalidSocket) {
                    return false;
                }

                std::unique_ptr<IocpUdpReadOperation> operation;
                IocpUdpReadOperation* raw = nullptr;
                try {
                    operation = std::make_unique<IocpUdpReadOperation>(&state, state.m_fd);
                    raw = operation.get();
                    raw->m_bufferView.buf = reinterpret_cast<char*>(
                        raw->m_buffer.PrepareWrite(kIocpUdpWireCapacity));
                    raw->m_bufferView.len = static_cast<ULONG>(kIocpUdpWireCapacity);
                    raw->m_peerLength = static_cast<SocketLength>(sizeof(raw->m_peer));
                    m_operations.emplace(&raw->m_overlapped, std::move(operation));
                    state.m_readOperation = raw;
                    state.m_readOutstanding = true;
                    state.m_readCancelRequested = false;
                }
                catch (...) {
                    if (raw != nullptr) m_operations.erase(&raw->m_overlapped);
                    state.m_readOperation = nullptr;
                    state.m_readOutstanding = false;
                    ::WSASetLastError(WSAENOBUFS);
                    return false;
                }

                DWORD received = 0; // WSARecvFrom 同步完成时的输出槽
                if (::WSARecvFrom(
                        state.m_fd,
                        &raw->m_bufferView,
                        1,
                        &received,
                        &raw->m_flags,
                        reinterpret_cast<sockaddr*>(&raw->m_peer),
                        &raw->m_peerLength,
                        &raw->m_overlapped,
                        nullptr) == SOCKET_ERROR) {
                    const int error = ::WSAGetLastError();
                    if (error != WSA_IO_PENDING) {
                        raw->m_syntheticCompletion = true;
                        raw->m_syntheticError = error;
                        if (!::PostQueuedCompletionStatus(
                                m_completionPort,
                                0,
                                state.m_completionKey,
                                &raw->m_overlapped)) {
                            m_operations.erase(&raw->m_overlapped);
                            state.m_readOperation = nullptr;
                            state.m_readOutstanding = false;
                            ::WSASetLastError(::GetLastError());
                            return false;
                        }
                    }
                }
                ++m_readSubmissions;
                return true;
            }

            bool IocpPollerImpl::SubmitUdpWrite(IocpConnectionState& state) noexcept {
                if (!m_activated || state.m_closing || state.m_writeOutstanding
                    || state.m_fd == kInvalidSocket || state.m_datagramWrites.empty()) {
                    return false;
                }

                IocpDatagramWriteState& datagram = state.m_datagramWrites.front();
                const std::size_t payloadBytes = datagram.m_buffer.ReadableBytes();
                if (payloadBytes > (std::numeric_limits<ULONG>::max)()) {
                    ::WSASetLastError(WSAEMSGSIZE);
                    return false;
                }

                std::unique_ptr<IocpUdpWriteOperation> operation;
                IocpUdpWriteOperation* raw = nullptr;
                try {
                    operation = std::make_unique<IocpUdpWriteOperation>(
                        &state,
                        state.m_fd,
                        &datagram);
                    raw = operation.get();
                    raw->m_bufferView.buf = reinterpret_cast<char*>(
                        const_cast<std::uint8_t*>(datagram.m_buffer.Peek()));
                    raw->m_bufferView.len = static_cast<ULONG>(payloadBytes);
                    m_operations.emplace(&raw->m_overlapped, std::move(operation));
                    state.m_writeOperation = raw;
                    state.m_writeOutstanding = true;
                    state.m_writeCancelRequested = false;
                }
                catch (...) {
                    if (raw != nullptr) m_operations.erase(&raw->m_overlapped);
                    state.m_writeOperation = nullptr;
                    state.m_writeOutstanding = false;
                    ::WSASetLastError(WSAENOBUFS);
                    return false;
                }

                DWORD sent = 0; // WSASend/WSASendTo 同步完成时的输出槽
                int result = SOCKET_ERROR;
                if (datagram.m_connected) {
                    result = ::WSASend(
                        state.m_fd,
                        &raw->m_bufferView,
                        1,
                        &sent,
                        0,
                        &raw->m_overlapped,
                        nullptr);
                }
                else {
                    result = ::WSASendTo(
                        state.m_fd,
                        &raw->m_bufferView,
                        1,
                        &sent,
                        0,
                        datagram.m_peer.SockAddr(),
                        datagram.m_peer.Length(),
                        &raw->m_overlapped,
                        nullptr);
                }
                if (result == SOCKET_ERROR) {
                    const int error = ::WSAGetLastError();
                    if (error != WSA_IO_PENDING) {
                        raw->m_syntheticCompletion = true;
                        raw->m_syntheticError = error;
                        if (!::PostQueuedCompletionStatus(
                                m_completionPort,
                                0,
                                state.m_completionKey,
                                &raw->m_overlapped)) {
                            m_operations.erase(&raw->m_overlapped);
                            state.m_writeOperation = nullptr;
                            state.m_writeOutstanding = false;
                            ::WSASetLastError(::GetLastError());
                            return false;
                        }
                    }
                }
                return true;
            }

            void IocpPollerImpl::CompleteTcpRead(
                IocpTcpReadOperation& operation,
                const OVERLAPPED_ENTRY& entry) noexcept {
                IocpConnectionState* state = operation.m_state;
                if (state == nullptr || state->m_connection == nullptr) return;
                Connection* connectionKey = state->m_connection.get();
                const bool wasCancelRequested = state->m_readCancelRequested; // 区分背压取消与真实错误
                state->m_readOperation = nullptr;
                state->m_readOutstanding = false;
                state->m_readCancelRequested = false;

                DWORD transferred = entry.dwNumberOfBytesTransferred;
                int error = 0;
                if (operation.m_syntheticCompletion) {
                    error = operation.m_syntheticError;
                }
                else if (!state->m_closing) {
                    DWORD flags = 0;
                    DWORD bytes = 0;
                    if (!::WSAGetOverlappedResult(
                            state->m_fd,
                            &operation.m_overlapped,
                            &bytes,
                            FALSE,
                            &flags)) {
                        error = ::WSAGetLastError();
                    }
                    else {
                        transferred = bytes;
                    }
                }

                if (!state->m_closing) {
                    const std::shared_ptr<Connection> snapshot = state->m_connection;
                    if (error == 0 && transferred > 0
                        && transferred <= operation.m_bufferView.len) {
                        operation.m_buffer.HasWritten(transferred);
                        PollerAccess::CompleteRead(*snapshot, std::move(operation.m_buffer));
                    }
                    else if (error == 0 && transferred == 0) {
                        PollerAccess::PeerClosed(*snapshot);
                    }
                    else if (error == WSA_OPERATION_ABORTED && wasCancelRequested) {
                        // PauseReading 的取消只等待 terminal packet，不得转成连接错误。
                    }
                    else if (error == 0 || error == WSA_OPERATION_ABORTED) {
                        PollerAccess::Error(*snapshot, EPROTO);
                    }
                    else {
                        PollerAccess::Error(*snapshot, error);
                    }
                }

                const auto found = m_connectionStates.find(connectionKey);
                if (found == m_connectionStates.end()) return;
                IocpConnectionState& current = *found->second;
                if (current.m_closing) {
                    CleanupClosedConnection(connectionKey);
                    return;
                }
                if (!current.m_readOutstanding && current.m_readEnabled
                    && PollerAccess::ReadEnabled(*current.m_connection)
                    && !SubmitTcpRead(current)) {
                    PollerAccess::Error(*current.m_connection, EIO);
                }
            }

            void IocpPollerImpl::CompleteTcpWrite(
                IocpTcpWriteOperation& operation,
                const OVERLAPPED_ENTRY& entry) noexcept {
                IocpConnectionState* state = operation.m_state;
                if (state == nullptr || state->m_connection == nullptr) return;
                Connection* connectionKey = state->m_connection.get();
                state->m_writeOperation = nullptr;
                state->m_writeOutstanding = false;
                state->m_writeCancelRequested = false;

                DWORD transferred = entry.dwNumberOfBytesTransferred;
                int error = 0;
                if (operation.m_syntheticCompletion) {
                    error = operation.m_syntheticError;
                }
                else if (!state->m_closing) {
                    DWORD flags = 0;
                    DWORD bytes = 0;
                    if (!::WSAGetOverlappedResult(
                            state->m_fd,
                            &operation.m_overlapped,
                            &bytes,
                            FALSE,
                            &flags)) {
                        error = ::WSAGetLastError();
                    }
                    else {
                        transferred = bytes;
                    }
                }

                if (!state->m_closing) {
                    const std::shared_ptr<Connection> snapshot = state->m_connection;
                    if (error == 0 && transferred > 0
                        && transferred <= operation.m_submittedBytes
                        && transferred <= state->m_pendingWriteBytes) {
                        state->m_writeChain.Consume(transferred);
                        state->m_pendingWriteBytes -= transferred;
                        PollerAccess::WriteDrain(*snapshot, state->m_pendingWriteBytes);

                        const auto current = m_connectionStates.find(connectionKey);
                        if (current != m_connectionStates.end()
                            && !current->second->m_closing) {
                            if (current->second->m_writeChain.Empty()) {
                                PollerAccess::WriteComplete(*current->second->m_connection);
                            }
                            else if (!current->second->m_writeOutstanding
                                && !SubmitTcpWrite(*current->second)) {
                                PollerAccess::Error(*current->second->m_connection, EIO);
                            }
                        }
                    }
                    else if (error == 0 || error == WSA_OPERATION_ABORTED) {
                        PollerAccess::Error(*snapshot, EPROTO);
                    }
                    else {
                        PollerAccess::Error(*snapshot, error);
                    }
                }

                CleanupClosedConnection(connectionKey);
            }

            void IocpPollerImpl::CompleteUdpRead(
                IocpUdpReadOperation& operation,
                const OVERLAPPED_ENTRY& entry) noexcept {
                IocpConnectionState* state = operation.m_state;
                if (state == nullptr || state->m_connection == nullptr) return;
                Connection* connectionKey = state->m_connection.get();
                const bool wasCancelRequested = state->m_readCancelRequested; // 暂停/关闭取消原因
                state->m_readOperation = nullptr;
                state->m_readOutstanding = false;
                state->m_readCancelRequested = false;

                DWORD transferred = entry.dwNumberOfBytesTransferred;
                int error = 0;
                if (operation.m_syntheticCompletion) {
                    error = operation.m_syntheticError;
                }
                else if (!state->m_closing) {
                    DWORD flags = 0;
                    DWORD bytes = 0;
                    if (!::WSAGetOverlappedResult(
                            state->m_fd,
                            &operation.m_overlapped,
                            &bytes,
                            FALSE,
                            &flags)) {
                        error = ::WSAGetLastError();
                    }
                    else {
                        transferred = bytes;
                    }
                }

                if (!state->m_closing) {
                    const std::shared_ptr<Connection> snapshot = state->m_connection;
                    if (error == 0 && transferred <= kIocpUdpWireCapacity) {
                        operation.m_buffer.HasWritten(transferred);
                        const std::size_t originalBytes = static_cast<std::size_t>(transferred);
                        const std::size_t capacity = (std::min)(
                            state->m_maxDatagramBytes,
                            kIocpUdpWireCapacity);
                        const bool truncated = originalBytes > capacity;
                        Buffer input(0); // 截断路径复制业务容量，完整路径直接移动 wire buffer
                        if (!truncated) {
                            input = std::move(operation.m_buffer);
                        }
                        else {
                            input.Append(operation.m_buffer.Peek(), capacity);
                        }

                        Address peer(operation.m_peer, operation.m_peerLength);
                        if (!peer.IsValid() && snapshot->GetRemoteAddress().IsValid()) {
                            peer = snapshot->GetRemoteAddress();
                        }
                        if (!peer.IsValid()) {
                            PollerAccess::Error(*snapshot, EPROTO);
                        }
                        else {
                            ++m_datagramReceiveCompletions;
                            ++m_receivedDatagrams;
                            if (originalBytes == 0) ++m_zeroLengthDatagrams;
                            if (truncated) ++m_truncatedDatagrams;
                            PollerAccess::CompleteDatagram(
                                *snapshot,
                                input,
                                peer,
                                originalBytes,
                                truncated);
                        }
                    }
                    else if (error == WSA_OPERATION_ABORTED && wasCancelRequested) {
                        // PauseReading 的 UDP cancel 只等待 terminal packet，不发布连接错误。
                    }
                    else if (error != 0) {
                        PollerAccess::Error(*snapshot, error);
                    }
                    else {
                        PollerAccess::Error(*snapshot, EPROTO);
                    }
                }

                const auto found = m_connectionStates.find(connectionKey);
                if (found == m_connectionStates.end()) return;
                IocpConnectionState& current = *found->second;
                if (current.m_closing) {
                    CleanupClosedConnection(connectionKey);
                    return;
                }
                if (!current.m_readOutstanding && current.m_readEnabled
                    && PollerAccess::ReadEnabled(*current.m_connection)) {
                    if (!SubmitUdpRead(current)) PollerAccess::Error(*current.m_connection, EIO);
                }
            }

            void IocpPollerImpl::CompleteUdpWrite(
                IocpUdpWriteOperation& operation,
                const OVERLAPPED_ENTRY& entry) noexcept {
                IocpConnectionState* state = operation.m_state;
                if (state == nullptr || state->m_connection == nullptr) return;
                Connection* connectionKey = state->m_connection.get();
                state->m_writeOperation = nullptr;
                state->m_writeOutstanding = false;
                state->m_writeCancelRequested = false;

                DWORD transferred = entry.dwNumberOfBytesTransferred;
                int error = 0;
                if (operation.m_syntheticCompletion) {
                    error = operation.m_syntheticError;
                }
                else if (!state->m_closing) {
                    DWORD flags = 0;
                    DWORD bytes = 0;
                    if (!::WSAGetOverlappedResult(
                            state->m_fd,
                            &operation.m_overlapped,
                            &bytes,
                            FALSE,
                            &flags)) {
                        error = ::WSAGetLastError();
                    }
                    else {
                        transferred = bytes;
                    }
                }

                if (!state->m_closing) {
                    const std::shared_ptr<Connection> snapshot = state->m_connection;
                    IocpDatagramWriteState* datagram = operation.m_datagram;
                    const std::size_t payloadBytes = datagram == nullptr
                        ? 0
                        : datagram->m_buffer.ReadableBytes();
                    if (error == 0 && datagram != nullptr && transferred == payloadBytes) {
                        const Address peer = datagram->m_peer;
                        const std::size_t queueCost = datagram->m_queueCost;
                        const bool zeroLength = payloadBytes == 0;
                        state->m_datagramWrites.pop_front();
                        state->m_pendingWriteBytes -= (std::min)(
                            queueCost,
                            state->m_pendingWriteBytes);
                        if (m_pendingDatagramSends > 0) --m_pendingDatagramSends;
                        ++m_datagramSendCompletions;
                        ++m_sentDatagrams;
                        if (zeroLength) ++m_zeroLengthDatagrams;
                        PollerAccess::WriteDrain(*snapshot, state->m_pendingWriteBytes);
                        PollerAccess::DatagramWriteCompleted(*snapshot, peer, queueCost);

                        const auto current = m_connectionStates.find(connectionKey);
                        if (current != m_connectionStates.end() && !current->second->m_closing) {
                            if (current->second->m_datagramWrites.empty()) {
                                PollerAccess::WriteComplete(*current->second->m_connection);
                            }
                            else if (!current->second->m_writeOutstanding
                                && !SubmitUdpWrite(*current->second)) {
                                PollerAccess::Error(*current->second->m_connection, EIO);
                            }
                        }
                    }
                    else {
                        PollerAccess::Error(
                            *snapshot,
                            error != 0 ? error : WSAEMSGSIZE);
                    }
                }

                CleanupClosedConnection(connectionKey);
            }

            void IocpPollerImpl::CancelConnectionOperation(IocpOperation& operation) noexcept {
                if (operation.m_fd == kInvalidSocket) return;
                if (!::CancelIoEx(
                        reinterpret_cast<HANDLE>(operation.m_fd),
                        &operation.m_overlapped)) {
                    const DWORD error = ::GetLastError();
                    if (error != ERROR_NOT_FOUND) ::SetLastError(error);
                }
            }

            void IocpPollerImpl::CleanupClosedConnection(Connection* connection) noexcept {
                if (connection == nullptr) return;
                const auto found = m_connectionStates.find(connection);
                if (found == m_connectionStates.end()) return;
                IocpConnectionState& state = *found->second;
                if (!state.m_closing || state.m_readOutstanding || state.m_writeOutstanding) return;
                state.m_writeChain.Clear();
                state.m_pendingWriteBytes = 0;
                const std::size_t abandonedDatagrams = state.m_datagramWrites.size();
                state.m_datagramWrites.clear();
                m_pendingDatagramSends = abandonedDatagrams >= m_pendingDatagramSends
                    ? 0
                    : m_pendingDatagramSends - abandonedDatagrams;
                m_connectionStates.erase(found);
            }

            IocpPoller::IocpPoller(EventLoop* ownerLoop)
                : Poller(ownerLoop), m_impl(new IocpPollerImpl{}) {
            }

            IocpPoller::~IocpPoller() {
                if (m_impl == nullptr) return;
                for (auto& item : m_impl->m_channelStates) {
                    RetireChannelState(*item.second);
                }
                m_impl->m_channelRegistrationIds.clear();
                m_impl->m_channelStates.clear();
                m_impl->m_connectStates.clear();
                m_impl->m_acceptStates.clear();
                for (auto& item : m_impl->m_connectionStates) {
                    if (item.second && item.second->m_connection) {
                        PollerAccess::Detach(*item.second->m_connection);
                    }
                }
                m_impl->m_connectionStates.clear();
                for (auto& item : m_impl->m_operations) {
                    if (!item.second) continue;
                    if (item.second->m_kind == IocpOperationKind::Accept) {
                        auto* accept = static_cast<IocpAcceptOperation*>(item.second.get());
                        Internal::CloseSocket(accept->m_acceptedFd);
                        accept->m_acceptedFd = kInvalidSocket;
                    }
                    else if (item.second->m_kind == IocpOperationKind::TcpRead
                        || item.second->m_kind == IocpOperationKind::TcpWrite
                        || item.second->m_kind == IocpOperationKind::UdpRead
                        || item.second->m_kind == IocpOperationKind::UdpWrite) {
                        // Connection state 已解除 Poller 绑定，由 Connection 自己关闭 socket；
                        // operation 只保留可析构标记，禁止析构阶段重复关闭数值 fd。
                        item.second->m_fd = kInvalidSocket;
                    }
                    else if (item.second->m_fd != kInvalidSocket) {
                        Internal::CloseSocket(item.second->m_fd);
                        item.second->m_fd = kInvalidSocket;
                    }
                }
                m_impl->m_operations.clear();
                m_impl->m_timeouts.clear();
                while (!m_impl->m_timeoutHeap.empty()) m_impl->m_timeoutHeap.pop();
                if (m_impl->m_completionPort != nullptr) {
                    (void)::CloseHandle(m_impl->m_completionPort);
                    m_impl->m_completionPort = nullptr;
                }
                delete m_impl;
                m_impl = nullptr;
            }

            bool IocpPoller::Activate() {
                if (m_impl == nullptr) return false;
                if (m_impl->m_activated) return true;

                try {
                    SocketRuntime::Ensure();
                }
                catch (...) {
                    SetLastError(::WSAGetLastError());
                    return false;
                }

                m_impl->m_completionPort = ::CreateIoCompletionPort(
                    INVALID_HANDLE_VALUE,
                    nullptr,
                    0,
                    0);
                if (m_impl->m_completionPort == nullptr) {
                    SetLastError(static_cast<int>(::GetLastError()));
                    return false;
                }
                m_impl->m_activated = true;
                for (auto& item : m_impl->m_channelStates) {
                    if (!ArmChannelState(*item.second)) {
                        const int error = LastError();
                        for (auto& active : m_impl->m_channelStates) {
                            RetireChannelState(*active.second);
                        }
                        m_impl->m_activated = false;
                        (void)::CloseHandle(m_impl->m_completionPort);
                        m_impl->m_completionPort = nullptr;
                        SetLastError(error);
                        return false;
                    }
                }
                return true;
            }

            bool IocpPoller::ArmChannelState(IocpChannelState& state) noexcept {
                if (m_impl == nullptr || !m_impl->m_activated
                    || state.m_fd == kInvalidSocket) {
                    return false;
                }
                if (state.m_wait != nullptr) return true;

                state.m_completionPort = m_impl->m_completionPort;
                state.m_event = ::WSACreateEvent();
                if (state.m_event == WSA_INVALID_EVENT) {
                    SetLastError(::WSAGetLastError());
                    return false;
                }
                if (::WSAEventSelect(
                        state.m_fd,
                        state.m_event,
                        state.m_networkMask) != 0) {
                    SetLastError(::WSAGetLastError());
                    ::WSACloseEvent(state.m_event);
                    state.m_event = WSA_INVALID_EVENT;
                    return false;
                }
                state.m_wait = ::CreateThreadpoolWait(
                    &ChannelWaitCallback,
                    &state,
                    nullptr);
                if (state.m_wait == nullptr) {
                    SetLastError(static_cast<int>(::GetLastError()));
                    (void)::WSAEventSelect(state.m_fd, WSA_INVALID_EVENT, 0);
                    ::WSACloseEvent(state.m_event);
                    state.m_event = WSA_INVALID_EVENT;
                    return false;
                }
                ::SetThreadpoolWait(state.m_wait, state.m_event, nullptr);
                return true;
            }

            void IocpPoller::RetireChannelState(IocpChannelState& state) noexcept {
                if (state.m_wait != nullptr) {
                    ::SetThreadpoolWait(state.m_wait, nullptr, nullptr);
                    ::WaitForThreadpoolWaitCallbacks(state.m_wait, TRUE);
                    ::CloseThreadpoolWait(state.m_wait);
                    state.m_wait = nullptr;
                }
                if (state.m_fd != kInvalidSocket) {
                    (void)::WSAEventSelect(state.m_fd, WSA_INVALID_EVENT, 0);
                }
                if (state.m_event != WSA_INVALID_EVENT) {
                    ::WSACloseEvent(state.m_event);
                    state.m_event = WSA_INVALID_EVENT;
                }
                state.m_completionPort = nullptr;
            }

            bool IocpPoller::AssociateSocket(SocketType fd, ULONG_PTR completionKey) noexcept {
                if (m_impl == nullptr || !m_impl->m_activated || fd == kInvalidSocket) {
                    return false;
                }
                HANDLE result = ::CreateIoCompletionPort(
                    reinterpret_cast<HANDLE>(fd),
                    m_impl->m_completionPort,
                    completionKey,
                    0);
                if (result != m_impl->m_completionPort) {
                    SetLastError(static_cast<int>(::GetLastError()));
                    return false;
                }
                return true;
            }

            bool IocpPoller::EnsureConnectEx() noexcept {
                if (m_impl == nullptr) return false;
                if (m_impl->m_connectExLoaded) return m_impl->m_connectEx != nullptr;

                m_impl->m_connectExLoaded = true;
                const SocketType probe = Internal::CreateSocket(
                    AF_INET,
                    SOCK_STREAM,
                    IPPROTO_TCP);
                if (probe == kInvalidSocket) {
                    m_impl->m_connectExError = Internal::GetLastSocketError();
                    return false;
                }

                GUID connectExGuid = WSAID_CONNECTEX;
                DWORD bytes = 0;
                LPFN_CONNECTEX connectEx = nullptr;
                const int result = ::WSAIoctl(
                    probe,
                    SIO_GET_EXTENSION_FUNCTION_POINTER,
                    &connectExGuid,
                    static_cast<DWORD>(sizeof(connectExGuid)),
                    &connectEx,
                    static_cast<DWORD>(sizeof(connectEx)),
                    &bytes,
                    nullptr,
                    nullptr);
                if (result == SOCKET_ERROR || connectEx == nullptr) {
                    m_impl->m_connectExError = Internal::GetLastSocketError();
                    Internal::CloseSocket(probe);
                    return false;
                }
                Internal::CloseSocket(probe);
                m_impl->m_connectEx = connectEx;
                return true;
            }

            bool IocpPoller::EnsureAcceptEx(SocketType listenFd) noexcept {
                if (m_impl == nullptr || listenFd == kInvalidSocket) return false;
                if (m_impl->m_acceptEx != nullptr) return true;

                GUID acceptExGuid = WSAID_ACCEPTEX; // Winsock 扩展函数标识
                DWORD bytes = 0; // WSAIoctl 返回的函数指针字节数
                LPFN_ACCEPTEX acceptEx = nullptr; // provider 返回的 AcceptEx 入口
                if (::WSAIoctl(
                        listenFd,
                        SIO_GET_EXTENSION_FUNCTION_POINTER,
                        &acceptExGuid,
                        static_cast<DWORD>(sizeof(acceptExGuid)),
                        &acceptEx,
                        static_cast<DWORD>(sizeof(acceptEx)),
                        &bytes,
                        nullptr,
                        nullptr) == SOCKET_ERROR
                    || acceptEx == nullptr) {
                    m_impl->m_acceptExError = Internal::GetLastSocketError();
                    return false;
                }
                m_impl->m_acceptEx = acceptEx;
                return true;
            }

            bool IocpPoller::SubmitAccept(IocpAcceptState& state) noexcept {
                if (m_impl == nullptr || m_impl->m_acceptEx == nullptr
                    || m_impl->m_completionPort == nullptr || state.m_closing) {
                    return false;
                }
                // accepted socket 必须与 listener 地址族一致，失败时保守回退 IPv4。
                const int family = [&state]() noexcept {
                    sockaddr_storage address{}; // listener 本地地址快照
                    SocketLength length = static_cast<SocketLength>(sizeof(address)); // 地址缓冲长度
                    if (::getsockname(
                            state.m_listenFd,
                            reinterpret_cast<sockaddr*>(&address),
                            &length) != 0) {
                        return AF_INET;
                    }
                    return static_cast<int>(address.ss_family);
                }();
                const SocketType acceptedFd = Internal::CreateSocket(
                    family,
                    SOCK_STREAM,
                    IPPROTO_TCP); // completion 前由新 operation 独占
                if (acceptedFd == kInvalidSocket) {
                    SetLastError(Internal::GetLastSocketError());
                    return false;
                }

                std::unique_ptr<IocpAcceptOperation> operation; // 等待进入统一 operation map
                IocpAcceptOperation* raw = nullptr; // 内核 terminal packet 前保持稳定地址
                try {
                    operation = std::make_unique<IocpAcceptOperation>(
                        state.m_listenFd,
                        acceptedFd,
                        state.m_callback);
                    raw = operation.get();
                    raw->m_id = m_impl->m_nextOperationId++;
                    raw->m_ownerGeneration = m_impl->m_nextGeneration++;
                    m_impl->m_operations.emplace(&raw->m_overlapped, std::move(operation));
                    state.m_operation = raw;
                }
                catch (...) {
                    if (raw != nullptr) m_impl->m_operations.erase(&raw->m_overlapped);
                    Internal::CloseSocket(acceptedFd);
                    SetLastError(WSAENOBUFS);
                    return false;
                }

                DWORD bytesReceived = 0; // AcceptEx 不预读 payload，保留 API 输出槽
                const BOOL accepted = m_impl->m_acceptEx(
                    state.m_listenFd,
                    raw->m_acceptedFd,
                    raw->m_addressBuffer.data(),
                    0,
                    static_cast<DWORD>(sizeof(sockaddr_storage) + 16),
                    static_cast<DWORD>(sizeof(sockaddr_storage) + 16),
                    &bytesReceived,
                    &raw->m_overlapped);
                if (!accepted) {
                    const int error = Internal::GetLastSocketError(); // pending 或同步 issuer 错误
                    if (error != WSA_IO_PENDING) {
                        raw->m_syntheticCompletion = true;
                        raw->m_syntheticError = error;
                        if (!::PostQueuedCompletionStatus(
                            m_impl->m_completionPort,
                            0,
                            0,
                            &raw->m_overlapped)) {
                            state.m_operation = nullptr;
                            m_impl->m_operations.erase(&raw->m_overlapped);
                            Internal::CloseSocket(acceptedFd);
                            SetLastError(static_cast<int>(::GetLastError()));
                            return false;
                        }
                    }
                }
                ++m_impl->m_acceptSubmissions;
                return true;
            }

            void IocpPoller::CompleteConnectOperation(
                IocpOperation& operation,
                bool syntheticCompletion) noexcept {
                if (m_impl == nullptr) return;
                auto* connect = static_cast<IocpConnectOperation*>(&operation);
                const auto found = m_impl->m_connectStates.find(connect->m_connectId);
                if (found == m_impl->m_connectStates.end()) {
                    Internal::CloseSocket(connect->m_fd);
                    connect->m_fd = kInvalidSocket;
                    return;
                }
                m_impl->m_connectStates.erase(found);

                if (operation.m_cancelRequested) {
                    Internal::CloseSocket(connect->m_fd);
                    connect->m_fd = kInvalidSocket;
                    return;
                }

                int error = syntheticCompletion ? operation.m_syntheticError : 0;
                if (!syntheticCompletion) {
                    DWORD bytes = 0;
                    DWORD flags = 0;
                    if (!::WSAGetOverlappedResult(
                        connect->m_fd,
                        &connect->m_overlapped,
                        &bytes,
                        FALSE,
                        &flags)) {
                        error = Internal::GetLastSocketError();
                    }
                }
                if (error == 0
                    && ::setsockopt(
                        connect->m_fd,
                        SOL_SOCKET,
                        SO_UPDATE_CONNECT_CONTEXT,
                        nullptr,
                        0) != 0) {
                    error = Internal::GetLastSocketError();
                }

                SocketType callbackSocket = connect->m_fd;
                if (error != 0) {
                    Internal::CloseSocket(connect->m_fd);
                    callbackSocket = kInvalidSocket;
                }
                else {
                    connect->m_fd = kInvalidSocket; // 成功 socket 所有权交给 callback
                }
                if (error == 0 && callbackSocket != kInvalidSocket) {
                    try {
                        m_impl->m_preAssociatedSockets.emplace(
                            callbackSocket,
                            EncodeSocketKey(connect->m_ownerGeneration));
                    }
                    catch (...) {
                        error = WSAEINVAL;
                        Internal::CloseSocket(callbackSocket);
                        callbackSocket = kInvalidSocket;
                    }
                }
                if (connect->m_callback) {
                    try {
                        connect->m_callback(callbackSocket, error);
                    }
                    catch (...) {
                        if (callbackSocket != kInvalidSocket) {
                            Internal::CloseSocket(callbackSocket);
                        }
                    }
                }
                if (callbackSocket != kInvalidSocket) {
                    m_impl->m_preAssociatedSockets.erase(callbackSocket); // 未被 Connection 接管时清理临时关联记录
                }
            }

            void IocpPoller::CompleteAcceptOperation(
                IocpOperation& operation,
                bool syntheticCompletion) noexcept {
                if (m_impl == nullptr) return;
                auto* accept = static_cast<IocpAcceptOperation*>(&operation); // 已按 kind 校验
                const auto found = m_impl->m_acceptStates.find(accept->m_listenFd); // 活跃 listener
                if (found == m_impl->m_acceptStates.end()) {
                    Internal::CloseSocket(accept->m_acceptedFd);
                    accept->m_acceptedFd = kInvalidSocket;
                    return;
                }
                IocpAcceptState& state = found->second; // terminal packet 对应的 issuer 状态
                state.m_operation = nullptr;

                int error = syntheticCompletion ? operation.m_syntheticError : 0; // 当前 accept 终态
                if (!syntheticCompletion) {
                    DWORD bytes = 0; // AcceptEx 未预读业务 payload
                    DWORD flags = 0; // Winsock completion 标志占位
                    if (!::WSAGetOverlappedResult(
                        accept->m_listenFd,
                        &accept->m_overlapped,
                        &bytes,
                        FALSE,
                        &flags)) {
                        error = Internal::GetLastSocketError();
                    }
                }

                if (operation.m_cancelRequested || state.m_closing || error != 0) {
                    Internal::CloseSocket(accept->m_acceptedFd);
                    accept->m_acceptedFd = kInvalidSocket;
                    if (operation.m_cancelRequested || state.m_closing) {
                        if (state.m_closing) m_impl->m_acceptStates.erase(found);
                        return;
                    }
                    // 可恢复的 AcceptEx 错误不能让 listener 静默失去唯一 accept operation。
                    if (!SubmitAccept(state)) {
                        state.m_closing = true;
                        m_impl->m_acceptStates.erase(found);
                    }
                    return;
                }

                if (::setsockopt(
                        accept->m_acceptedFd,
                        SOL_SOCKET,
                        SO_UPDATE_ACCEPT_CONTEXT,
                        reinterpret_cast<const char*>(&accept->m_listenFd),
                        static_cast<int>(sizeof(accept->m_listenFd))) != 0) {
                    error = Internal::GetLastSocketError();
                    Internal::CloseSocket(accept->m_acceptedFd);
                    accept->m_acceptedFd = kInvalidSocket;
                }

                if (error != 0) {
                    // accepted socket 无法更新上下文时只丢弃当前连接，并继续监听。
                    if (!SubmitAccept(state)) {
                        state.m_closing = true;
                        m_impl->m_acceptStates.erase(found);
                    }
                    return;
                }

                // 先提交 replacement，再把 accepted socket 交给用户代码。
                if (!SubmitAccept(state)) {
                    state.m_closing = true;
                    Internal::CloseSocket(accept->m_acceptedFd);
                    accept->m_acceptedFd = kInvalidSocket;
                    m_impl->m_acceptStates.erase(found);
                    return;
                }

                SocketType callbackSocket = accept->m_acceptedFd; // 准备转交的 accepted socket
                if (state.m_closing) {
                    Internal::CloseSocket(callbackSocket);
                    callbackSocket = kInvalidSocket;
                }
                else {
                    accept->m_acceptedFd = kInvalidSocket;
                }
                if (callbackSocket != kInvalidSocket && accept->m_callback) {
                    try {
                        accept->m_callback(callbackSocket);
                    }
                    catch (...) {
                        Internal::CloseSocket(callbackSocket);
                    }
                }
            }

            bool IocpPoller::AddChannel(Channel* channel) {
                if (m_impl == nullptr || channel == nullptr
                    || channel->GetSocket() == kInvalidSocket) {
                    SetLastError(WSAEINVAL);
                    return false;
                }
                if (m_impl->m_channelRegistrationIds.find(channel)
                    != m_impl->m_channelRegistrationIds.end()) {
                    return UpdateChannel(channel);
                }
                if (!StoreChannel(channel)) return false;

                const std::uint64_t registrationId = m_impl->NextRegistrationId();
                if (registrationId == 0) {
                    (void)EraseChannel(channel);
                    SetLastError(WSAEINVAL);
                    return false;
                }
                auto state = std::make_unique<IocpChannelState>();
                state->m_registrationId = registrationId;
                state->m_channel = channel;
                state->m_fd = channel->GetSocket();
                state->m_networkMask = ToWindowsNetworkMask(channel->Events());
                try {
                    m_impl->m_channelStates.emplace(registrationId, std::move(state));
                    m_impl->m_channelRegistrationIds.emplace(channel, registrationId);
                    auto found = m_impl->m_channelStates.find(registrationId);
                    if (m_impl->m_activated && !ArmChannelState(*found->second)) {
                        m_impl->m_channelRegistrationIds.erase(channel);
                        m_impl->m_channelStates.erase(registrationId);
                        (void)EraseChannel(channel);
                        return false;
                    }
                }
                catch (...) {
                    m_impl->m_channelRegistrationIds.erase(channel);
                    m_impl->m_channelStates.erase(registrationId);
                    (void)EraseChannel(channel);
                    SetLastError(WSAENOBUFS);
                    return false;
                }

                channel->SetIndex(Channel::Index::Added);
                return true;
            }

            bool IocpPoller::RemoveChannel(Channel* channel) {
                if (m_impl == nullptr || channel == nullptr) return false;
                const auto foundId = m_impl->m_channelRegistrationIds.find(channel);
                if (foundId == m_impl->m_channelRegistrationIds.end()) return false;
                const std::uint64_t registrationId = foundId->second;
                m_impl->m_channelRegistrationIds.erase(foundId);
                const auto foundState = m_impl->m_channelStates.find(registrationId);
                if (foundState != m_impl->m_channelStates.end()) {
                    RetireChannelState(*foundState->second);
                    m_impl->m_channelStates.erase(foundState);
                }
                (void)EraseChannel(channel);
                channel->SetIndex(Channel::Index::Deleted);
                return true;
            }

            bool IocpPoller::UpdateChannel(Channel* channel) {
                if (m_impl == nullptr || channel == nullptr
                    || channel->GetSocket() == kInvalidSocket) {
                    return false;
                }
                const auto foundId = m_impl->m_channelRegistrationIds.find(channel);
                if (foundId == m_impl->m_channelRegistrationIds.end()) {
                    return AddChannel(channel);
                }

                const std::uint64_t oldId = foundId->second;
                auto foundState = m_impl->m_channelStates.find(oldId);
                if (foundState == m_impl->m_channelStates.end()) return false;
                std::unique_ptr<IocpChannelState> oldState = std::move(foundState->second);
                m_impl->m_channelStates.erase(foundState);
                m_impl->m_channelRegistrationIds.erase(foundId);
                RetireChannelState(*oldState);

                const std::uint64_t registrationId = m_impl->NextRegistrationId();
                if (registrationId == 0) {
                    SetLastError(WSAEINVAL);
                    return false;
                }
                auto state = std::make_unique<IocpChannelState>();
                state->m_registrationId = registrationId;
                state->m_channel = channel;
                state->m_fd = channel->GetSocket();
                state->m_networkMask = ToWindowsNetworkMask(channel->Events());
                try {
                    m_impl->m_channelStates.emplace(registrationId, std::move(state));
                    m_impl->m_channelRegistrationIds.emplace(channel, registrationId);
                    auto current = m_impl->m_channelStates.find(registrationId);
                    if (m_impl->m_activated && !ArmChannelState(*current->second)) {
                        m_impl->m_channelRegistrationIds.erase(channel);
                        m_impl->m_channelStates.erase(registrationId);
                        return false;
                    }
                }
                catch (...) {
                    m_impl->m_channelRegistrationIds.erase(channel);
                    m_impl->m_channelStates.erase(registrationId);
                    SetLastError(WSAENOBUFS);
                    return false;
                }
                return true;
            }

            void IocpPoller::ProcessChannelCompletion(
                std::uint64_t registrationId,
                std::vector<Channel*>& active,
                std::uint64_t& batchItems) noexcept {
                if (m_impl == nullptr) return;
                const auto found = m_impl->m_channelStates.find(registrationId);
                if (found == m_impl->m_channelStates.end()) {
                    ++m_impl->m_unknownPackets;
                    return;
                }
                IocpChannelState& state = *found->second;
                if (state.m_channel == nullptr || state.m_event == WSA_INVALID_EVENT) {
                    ++m_impl->m_unknownPackets;
                    return;
                }

                WSANETWORKEVENTS networkEvents{};
                IOEvent events = IOEvent::None;
                if (::WSAEnumNetworkEvents(
                        state.m_fd,
                        state.m_event,
                        &networkEvents) != 0) {
                    SetLastError(::WSAGetLastError());
                    events |= IOEvent::Error;
                }
                else {
                    if ((networkEvents.lNetworkEvents & FD_READ) != 0) {
                        events |= IOEvent::Read;
                    }
                    if ((networkEvents.lNetworkEvents & FD_WRITE) != 0) {
                        events |= IOEvent::Write;
                    }
                    if ((networkEvents.lNetworkEvents & FD_CLOSE) != 0) {
                        events |= IOEvent::Close;
                    }
                    for (int index = 0; index < FD_MAX_EVENTS; ++index) {
                        if (networkEvents.iErrorCode[index] != 0) {
                            events |= IOEvent::Error;
                            SetLastError(networkEvents.iErrorCode[index]);
                            break;
                        }
                    }
                }

                if (events != IOEvent::None) {
                    state.m_channel->SetRevents(events);
                    if (std::find(active.begin(), active.end(), state.m_channel)
                        == active.end()) {
                        active.push_back(state.m_channel);
                    }
                }
                if (state.m_wait != nullptr) {
                    ::SetThreadpoolWait(state.m_wait, state.m_event, nullptr);
                }
                ++m_impl->m_completedOperations;
                ++batchItems;
            }

            void IocpPoller::Poll(int timeoutMs, std::vector<Channel*>& active) {
                active.clear();
                if (m_impl == nullptr || !m_impl->m_activated) return;

                m_impl->PruneTimeoutHeap();
                const auto callerDeadline = std::chrono::steady_clock::now()
                    + std::chrono::milliseconds((std::max)(timeoutMs, 0));
                auto waitDeadline = callerDeadline;
                if (!m_impl->m_timeoutHeap.empty()) {
                    waitDeadline = (std::min)(
                        waitDeadline,
                        m_impl->m_timeoutHeap.top().m_deadline);
                }

                ULONG entryCount = 0;
                for (;;) {
                    const DWORD waitMs = WaitMillisecondsUntil(waitDeadline);
                    const BOOL waited = ::GetQueuedCompletionStatusEx(
                        m_impl->m_completionPort,
                        m_impl->m_entries.data(),
                        static_cast<ULONG>(m_impl->m_entries.size()),
                        &entryCount,
                        waitMs,
                        FALSE);
                    if (waited) break;

                    const DWORD error = ::GetLastError();
                    if (error == WAIT_TIMEOUT
                        && std::chrono::steady_clock::now() < waitDeadline) {
                        // Windows wait granularity may return a timeout slightly early;
                        // recompute the remaining absolute budget instead of dropping a timer.
                        continue;
                    }
                    if (error != WAIT_TIMEOUT) SetLastError(static_cast<int>(error));
                    entryCount = 0;
                    break;
                }

                std::uint64_t batchItems = 0;
                for (ULONG index = 0; index < entryCount; ++index) {
                    const OVERLAPPED_ENTRY& entry = m_impl->m_entries[index];
                    if (entry.lpOverlapped == nullptr
                        && entry.lpCompletionKey == 0) {
                        ++m_impl->m_completedOperations;
                        ++batchItems;
                        continue;
                    }
                    if (entry.lpOverlapped == nullptr
                        && (entry.lpCompletionKey & 1U) != 0) {
                        ProcessChannelCompletion(
                            static_cast<std::uint64_t>(entry.lpCompletionKey >> 1),
                            active,
                            batchItems);
                        continue;
                    }

                    const auto found = m_impl->m_operations.find(entry.lpOverlapped);
                    if (found == m_impl->m_operations.end()) {
                        ++m_impl->m_unknownPackets;
                        continue;
                    }

                    std::unique_ptr<IocpOperation> operation = std::move(found->second);
                    m_impl->m_operations.erase(found);
                    if (operation->m_kind == IocpOperationKind::Connect) {
                        CompleteConnectOperation(
                            *operation,
                            operation->m_syntheticCompletion);
                    }
                    else if (operation->m_kind == IocpOperationKind::Accept) {
                        CompleteAcceptOperation(
                            *operation,
                            operation->m_syntheticCompletion);
                    }
                    else if (operation->m_kind == IocpOperationKind::TcpRead) {
                        m_impl->CompleteTcpRead(
                            *static_cast<IocpTcpReadOperation*>(operation.get()),
                            entry);
                    }
                    else if (operation->m_kind == IocpOperationKind::TcpWrite) {
                        m_impl->CompleteTcpWrite(
                            *static_cast<IocpTcpWriteOperation*>(operation.get()),
                            entry);
                    }
                    else if (operation->m_kind == IocpOperationKind::UdpRead) {
                        m_impl->CompleteUdpRead(
                            *static_cast<IocpUdpReadOperation*>(operation.get()),
                            entry);
                    }
                    else if (operation->m_kind == IocpOperationKind::UdpWrite) {
                        m_impl->CompleteUdpWrite(
                            *static_cast<IocpUdpWriteOperation*>(operation.get()),
                            entry);
                    }
                    else if (!operation->m_syntheticCompletion
                        && operation->m_fd != kInvalidSocket) {
                        DWORD flags = 0;
                        DWORD bytes = 0;
                        if (!::WSAGetOverlappedResult(
                            operation->m_fd,
                            &operation->m_overlapped,
                            &bytes,
                            FALSE,
                            &flags)) {
                            SetLastError(::WSAGetLastError());
                        }
                    }
                    ++m_impl->m_completedOperations;
                    ++batchItems;
                }

                while (true) {
                    m_impl->PruneTimeoutHeap();
                    if (m_impl->m_timeoutHeap.empty()
                        || m_impl->m_timeoutHeap.top().m_deadline
                            > std::chrono::steady_clock::now()) {
                        break;
                    }

                    const IocpTimeoutNode node = m_impl->m_timeoutHeap.top();
                    m_impl->m_timeoutHeap.pop();
                    const auto found = m_impl->m_timeouts.find(node.m_id);
                    if (found == m_impl->m_timeouts.end()
                        || found->second.m_deadline != node.m_deadline) {
                        continue;
                    }
                    Poller::TimeoutCallback callback = std::move(found->second.m_callback);
                    m_impl->m_timeouts.erase(found);
                    ++m_impl->m_completedOperations;
                    ++batchItems;
                    if (callback) {
                        try {
                            callback();
                        }
                        catch (...) {
                            // Poller 不让单个 timer callback 击穿 completion 循环。
                        }
                    }
                }
                m_impl->RecordBatch(batchItems);
            }

            void IocpPoller::Flush() {
            }

            Poller::ConnectId IocpPoller::StartConnect(
                const Address& remoteAddress,
                ConnectCallback callback) noexcept {
                if (m_impl == nullptr || !m_impl->m_activated
                    || !remoteAddress.IsValid() || !callback) {
                    return InvalidConnectId;
                }
                if (!EnsureConnectEx()) {
                    SetLastError(m_impl->m_connectExError);
                    return InvalidConnectId;
                }

                const SocketType fd = Internal::CreateSocket(
                    remoteAddress.FamilyValue(),
                    SOCK_STREAM,
                    IPPROTO_TCP);
                if (fd == kInvalidSocket) {
                    SetLastError(Internal::GetLastSocketError());
                    return InvalidConnectId;
                }
                if (!Internal::SetTcpNoDelay(fd)) {
                    SetLastError(Internal::GetLastSocketError());
                    Internal::CloseSocket(fd);
                    return InvalidConnectId;
                }

                int bindResult = SOCKET_ERROR;
                if (remoteAddress.FamilyValue() == AF_INET) {
                    sockaddr_in local{};
                    local.sin_family = AF_INET;
                    local.sin_addr.s_addr = htonl(INADDR_ANY);
                    bindResult = Internal::BindSocket(
                        fd,
                        reinterpret_cast<const sockaddr*>(&local),
                        static_cast<SocketLength>(sizeof(local)));
                }
                else if (remoteAddress.FamilyValue() == AF_INET6) {
                    sockaddr_in6 local{};
                    local.sin6_family = AF_INET6;
                    local.sin6_addr = in6addr_any;
                    bindResult = Internal::BindSocket(
                        fd,
                        reinterpret_cast<const sockaddr*>(&local),
                        static_cast<SocketLength>(sizeof(local)));
                }
                if (bindResult != 0) {
                    SetLastError(Internal::GetLastSocketError());
                    Internal::CloseSocket(fd);
                    return InvalidConnectId;
                }

                const std::uint64_t generation = m_impl->m_nextGeneration++; // listener IOCP key
                ULONG_PTR completionKey = 0;
                try {
                    completionKey = EncodeSocketKey(generation);
                }
                catch (...) {
                    SetLastError(WSAEINVAL);
                    Internal::CloseSocket(fd);
                    return InvalidConnectId;
                }
                if (!AssociateSocket(fd, completionKey)) {
                    Internal::CloseSocket(fd);
                    return InvalidConnectId;
                }

                const ConnectId connectId = m_impl->NextConnectId();
                std::unique_ptr<IocpConnectOperation> operation;
                IocpConnectOperation* raw = nullptr;
                try {
                    operation = std::make_unique<IocpConnectOperation>(
                        connectId,
                        fd,
                        remoteAddress,
                        std::move(callback));
                    operation->m_id = m_impl->m_nextOperationId++;
                    operation->m_ownerGeneration = generation;
                    raw = operation.get();
                    m_impl->m_operations.emplace(&raw->m_overlapped, std::move(operation));
                    m_impl->m_connectStates.emplace(connectId, raw);
                }
                catch (...) {
                    if (raw != nullptr) {
                        m_impl->m_connectStates.erase(connectId);
                        m_impl->m_operations.erase(&raw->m_overlapped);
                    }
                    SetLastError(WSAENOBUFS);
                    Internal::CloseSocket(fd);
                    return InvalidConnectId;
                }

                auto stateFound = m_impl->m_connectStates.find(connectId);
                raw = stateFound->second;
                DWORD bytesSent = 0;
                BOOL accepted = m_impl->m_connectEx(
                    fd,
                    remoteAddress.SockAddr(),
                    remoteAddress.Length(),
                    nullptr,
                    0,
                    &bytesSent,
                    &raw->m_overlapped);
                if (!accepted) {
                    const int error = Internal::GetLastSocketError();
                    if (error != WSA_IO_PENDING) {
                        raw->m_syntheticCompletion = true;
                        raw->m_syntheticError = error;
                        if (!::PostQueuedCompletionStatus(
                            m_impl->m_completionPort,
                            0,
                            completionKey,
                            &raw->m_overlapped)) {
                            m_impl->m_connectStates.erase(connectId);
                            m_impl->m_operations.erase(&raw->m_overlapped);
                            Internal::CloseSocket(fd);
                            SetLastError(static_cast<int>(::GetLastError()));
                            return InvalidConnectId;
                        }
                    }
                }
                ++m_impl->m_connectSubmissions;
                return connectId;
            }

            void IocpPoller::CancelConnect(ConnectId connectId) noexcept {
                if (m_impl == nullptr || connectId == InvalidConnectId) return;
                const auto found = m_impl->m_connectStates.find(connectId);
                if (found == m_impl->m_connectStates.end()) return;
                IocpConnectOperation* operation = found->second;
                if (operation->m_cancelRequested) return;
                operation->m_cancelRequested = true;
                ++m_impl->m_connectCancelSubmissions;
                if (!::CancelIoEx(
                    reinterpret_cast<HANDLE>(operation->m_fd),
                    &operation->m_overlapped)) {
                    const DWORD error = ::GetLastError();
                    if (error != ERROR_NOT_FOUND) SetLastError(static_cast<int>(error));
                }
            }

            bool IocpPoller::StartConnection(const std::shared_ptr<Connection>& connection) {
                if (m_impl == nullptr || !m_impl->m_activated || !connection
                    || connection->GetSocket() == kInvalidSocket
                    || (connection->GetTransportKind() != TransportKind::Tcp
                        && connection->GetTransportKind() != TransportKind::Udp)) {
                    SetLastError(WSAEINVAL);
                    return false;
                }
                if (m_impl->m_connectionStates.find(connection.get())
                    != m_impl->m_connectionStates.end()) {
                    return true;
                }

                ULONG_PTR completionKey = 0;
                const auto preAssociated = m_impl->m_preAssociatedSockets.find(connection->GetSocket());
                if (preAssociated != m_impl->m_preAssociatedSockets.end()) {
                    completionKey = preAssociated->second; // ConnectEx 已完成关联，不能重复调用 CreateIoCompletionPort
                    m_impl->m_preAssociatedSockets.erase(preAssociated);
                }
                else {
                    const std::uint64_t generation = m_impl->m_nextGeneration++; // fd 复用时不复用身份
                    try {
                        completionKey = EncodeSocketKey(generation);
                    }
                    catch (...) {
                        SetLastError(WSAEINVAL);
                        return false;
                    }
                    if (!AssociateSocket(connection->GetSocket(), completionKey)) return false;
                }

                auto state = std::make_unique<IocpConnectionState>(); // state 持有 completion 期间的连接快照
                state->m_connection = connection;
                state->m_fd = connection->GetSocket();
                state->m_completionKey = completionKey;
                state->m_datagram = connection->GetTransportKind() == TransportKind::Udp;
                state->m_maxDatagramBytes = state->m_datagram
                    ? PollerAccess::MaxDatagramBytes(*connection)
                    : 0;
                state->m_readEnabled = PollerAccess::ReadEnabled(*connection);
                try {
                    const auto inserted = m_impl->m_connectionStates.emplace(
                        connection.get(),
                        std::move(state));
                    if (!inserted.second) {
                        SetLastError(WSAEALREADY);
                        return false;
                    }
                    if (inserted.first->second->m_readEnabled
                        && !(inserted.first->second->m_datagram
                            ? m_impl->SubmitUdpRead(*inserted.first->second)
                            : m_impl->SubmitTcpRead(*inserted.first->second))) {
                        m_impl->m_connectionStates.erase(inserted.first);
                        return false;
                    }
                }
                catch (...) {
                    m_impl->m_connectionStates.erase(connection.get());
                    SetLastError(WSAENOBUFS);
                    return false;
                }
                return true;
            }

            bool IocpPoller::StartAccept(SocketType listenFd, AcceptCallback callback) {
                if (m_impl == nullptr || !m_impl->m_activated
                    || listenFd == kInvalidSocket || !callback) {
                    return false;
                }
                if (m_impl->m_acceptStates.find(listenFd)
                    != m_impl->m_acceptStates.end()) {
                    return false;
                }
                if (!EnsureAcceptEx(listenFd)) {
                    SetLastError(m_impl->m_acceptExError);
                    return false;
                }

                const std::uint64_t generation = m_impl->m_nextGeneration++;
                try {
                    if (!AssociateSocket(listenFd, EncodeSocketKey(generation))) {
                        return false;
                    }
                }
                catch (...) {
                    SetLastError(WSAEINVAL);
                    return false;
                }

                IocpAcceptState state{}; // 插入状态表后才允许提交内核 operation
                state.m_listenFd = listenFd;
                state.m_callback = std::move(callback);
                try {
                    const auto inserted = m_impl->m_acceptStates.emplace(
                        listenFd,
                        std::move(state)); // 同一 listener 只允许一个 issuer 状态
                    if (!inserted.second || !SubmitAccept(inserted.first->second)) {
                        m_impl->m_acceptStates.erase(listenFd);
                        return false;
                    }
                }
                catch (...) {
                    m_impl->m_acceptStates.erase(listenFd);
                    SetLastError(WSAENOBUFS);
                    return false;
                }
                return true;
            }

            void IocpPoller::StopAccept(SocketType listenFd) {
                if (m_impl == nullptr || listenFd == kInvalidSocket) return;
                const auto found = m_impl->m_acceptStates.find(listenFd); // 当前 listener 状态
                if (found == m_impl->m_acceptStates.end()) return;
                IocpAcceptState& state = found->second; // terminal packet 前保持状态
                state.m_closing = true;
                if (state.m_operation != nullptr) {
                    state.m_operation->m_cancelRequested = true;
                    if (!::CancelIoEx(
                        reinterpret_cast<HANDLE>(listenFd),
                        &state.m_operation->m_overlapped)) {
                        const DWORD error = ::GetLastError(); // packet 已排队时允许 ERROR_NOT_FOUND
                        if (error != ERROR_NOT_FOUND) SetLastError(static_cast<int>(error));
                    }
                    return;
                }
                m_impl->m_acceptStates.erase(found);
            }

            Poller::TimeoutId IocpPoller::ScheduleTimeout(
                std::chrono::milliseconds delay,
                TimeoutCallback callback) noexcept {
                if (m_impl == nullptr || !m_impl->m_activated || !callback) {
                    return InvalidTimeoutId;
                }

                const TimeoutId timeoutId = m_impl->NextTimeoutId();
                if (timeoutId == InvalidTimeoutId) return InvalidTimeoutId;
                IocpTimeoutState state{};
                state.m_deadline = std::chrono::steady_clock::now() + delay;
                state.m_callback = std::move(callback);
                try {
                    m_impl->m_timeouts.emplace(timeoutId, std::move(state));
                    const auto found = m_impl->m_timeouts.find(timeoutId);
                    m_impl->m_timeoutHeap.push({ found->second.m_deadline, timeoutId });
                }
                catch (...) {
                    m_impl->m_timeouts.erase(timeoutId);
                    return InvalidTimeoutId;
                }
                return timeoutId;
            }

            void IocpPoller::CancelTimeout(TimeoutId timeoutId) noexcept {
                if (m_impl == nullptr || timeoutId == InvalidTimeoutId) return;
                m_impl->m_timeouts.erase(timeoutId);
            }

            bool IocpPoller::QueueWrite(Connection* connection, Buffer&& buffer) {
                BufferChain chain; // 单 Buffer 入口统一进入多段 owning 写链
                chain.Append(std::move(buffer));
                return QueueWrite(connection, std::move(chain));
            }

            bool IocpPoller::QueueWrite(Connection* connection, BufferChain&& chain) {
                if (m_impl == nullptr || connection == nullptr) return false;
                const auto found = m_impl->m_connectionStates.find(connection);
                if (found == m_impl->m_connectionStates.end()) return false;
                IocpConnectionState& state = *found->second;
                if (state.m_datagram) return false;
                const std::size_t incomingBytes = chain.ReadableBytes(); // 移动前保存背压统计
                if (state.m_closing || incomingBytes == 0) return true;
                if (incomingBytes > (std::numeric_limits<std::size_t>::max)()
                    - state.m_pendingWriteBytes) {
                    PollerAccess::Error(*state.m_connection, ENOBUFS);
                    return true;
                }

                try {
                    state.m_writeChain.Append(std::move(chain)); // 只转移 Buffer 所有权
                }
                catch (...) {
                    PollerAccess::Error(*state.m_connection, ENOMEM);
                    return true;
                }
                state.m_pendingWriteBytes += incomingBytes;
                const std::shared_ptr<Connection> snapshot = state.m_connection;
                if (!PollerAccess::WriteGrowth(*snapshot, state.m_pendingWriteBytes)) return true;

                const auto current = m_impl->m_connectionStates.find(connection);
                if (current == m_impl->m_connectionStates.end()
                    || current->second->m_closing) return true;
                if (!current->second->m_writeOutstanding
                    && !m_impl->SubmitTcpWrite(*current->second)) {
                    PollerAccess::Error(*current->second->m_connection, EIO);
                }
                return true;
            }

            bool IocpPoller::QueueDatagramWrite(
                Connection* connection,
                const Address& peer,
                Buffer&& buffer) {
                if (m_impl == nullptr || connection == nullptr) return false;
                const auto found = m_impl->m_connectionStates.find(connection);
                if (found == m_impl->m_connectionStates.end()
                    || !found->second->m_datagram) return false;
                IocpConnectionState& state = *found->second;
                if (state.m_closing) return true;

                const DatagramWriteTarget target = ResolveDatagramWriteTarget(
                    connection->GetRemoteAddress().IsValid(),
                    peer.IsValid());
                if (target == DatagramWriteTarget::Invalid) return false;
                const std::size_t payloadBytes = buffer.ReadableBytes();
                const std::size_t queueCost = DatagramQueueCost(payloadBytes);
                if (queueCost > (std::numeric_limits<std::size_t>::max)()
                    - state.m_pendingWriteBytes) {
                    PollerAccess::Error(*state.m_connection, ENOBUFS);
                    return true;
                }

                IocpDatagramWriteState datagram{}; // FIFO 节点先完整建立再入队
                datagram.m_peer = target == DatagramWriteTarget::Connected
                    ? connection->GetRemoteAddress()
                    : peer;
                datagram.m_buffer = std::move(buffer);
                datagram.m_queueCost = queueCost;
                datagram.m_connected = target == DatagramWriteTarget::Connected;
                try {
                    state.m_datagramWrites.push_back(std::move(datagram));
                }
                catch (...) {
                    PollerAccess::Error(*state.m_connection, ENOMEM);
                    return true;
                }
                state.m_pendingWriteBytes += queueCost;
                ++m_impl->m_pendingDatagramSends;
                const std::shared_ptr<Connection> snapshot = state.m_connection;
                if (!PollerAccess::WriteGrowth(*snapshot, state.m_pendingWriteBytes)) return true;

                const auto current = m_impl->m_connectionStates.find(connection);
                if (current == m_impl->m_connectionStates.end()
                    || current->second->m_closing) return true;
                if (!current->second->m_writeOutstanding
                    && !m_impl->SubmitUdpWrite(*current->second)) {
                    PollerAccess::Error(*current->second->m_connection, EIO);
                }
                return true;
            }

            void IocpPoller::SetReadEnabled(Connection* connection, bool enabled) {
                if (m_impl == nullptr || connection == nullptr) return;
                const auto found = m_impl->m_connectionStates.find(connection);
                if (found == m_impl->m_connectionStates.end()) return;
                IocpConnectionState& state = *found->second;
                if (state.m_closing || state.m_readEnabled == enabled) return;
                state.m_readEnabled = enabled;
                if (!enabled && state.m_readOutstanding && state.m_readOperation != nullptr
                    && !state.m_readCancelRequested) {
                    state.m_readCancelRequested = true;
                    m_impl->CancelConnectionOperation(*state.m_readOperation);
                }
                else if (enabled && !state.m_readOutstanding) {
                    const bool submitted = state.m_datagram
                        ? m_impl->SubmitUdpRead(state)
                        : m_impl->SubmitTcpRead(state);
                    if (!submitted) {
                        PollerAccess::Error(*state.m_connection, EIO);
                    }
                }
            }

            std::size_t IocpPoller::PendingWriteBytes(const Connection* connection) const noexcept {
                if (m_impl == nullptr || connection == nullptr) return 0;
                const auto found = m_impl->m_connectionStates.find(
                    const_cast<Connection*>(connection));
                return found == m_impl->m_connectionStates.end()
                    ? 0
                    : found->second->m_pendingWriteBytes;
            }

            void IocpPoller::StopConnection(Connection* connection) {
                if (m_impl == nullptr || connection == nullptr) return;
                const auto found = m_impl->m_connectionStates.find(connection);
                if (found == m_impl->m_connectionStates.end()) return;
                IocpConnectionState& state = *found->second;
                state.m_closing = true;
                state.m_readEnabled = false;
                if (state.m_readOutstanding && state.m_readOperation != nullptr
                    && !state.m_readCancelRequested) {
                    state.m_readCancelRequested = true;
                    m_impl->CancelConnectionOperation(*state.m_readOperation);
                }
                if (state.m_writeOutstanding && state.m_writeOperation != nullptr
                    && !state.m_writeCancelRequested) {
                    state.m_writeCancelRequested = true;
                    m_impl->CancelConnectionOperation(*state.m_writeOperation);
                }
                m_impl->CleanupClosedConnection(connection);
            }

            bool IocpPoller::HasPendingShutdownCompletions() const noexcept {
                return m_impl != nullptr && !m_impl->m_operations.empty();
            }

            std::uint64_t IocpPoller::CompletedOperationCount() const noexcept {
                return m_impl == nullptr ? 0 : m_impl->m_completedOperations;
            }

            std::size_t IocpPoller::ProvidedBufferCount() const noexcept {
                return 0;
            }

            std::uint64_t IocpPoller::ReadSubmissionCount() const noexcept {
                return m_impl == nullptr ? 0 : m_impl->m_readSubmissions;
            }

            std::uint64_t IocpPoller::AcceptSubmissionCount() const noexcept {
                return m_impl == nullptr ? 0 : m_impl->m_acceptSubmissions;
            }

            bool IocpPoller::ReceiveBundleEnabled() const noexcept {
                return false;
            }

            CompletionStats IocpPoller::GetCompletionStats() const noexcept {
                CompletionStats stats{};
                if (m_impl == nullptr) return stats;
                stats.completedOperations = m_impl->m_completedOperations;
                stats.connectSubmissions = m_impl->m_connectSubmissions;
                stats.connectCancelSubmissions = m_impl->m_connectCancelSubmissions;
                stats.pendingConnectOperations = m_impl->m_connectStates.size();
                stats.completionBatchCount = m_impl->m_completionBatchCount;
                stats.completionBatchItems = m_impl->m_completionBatchItems;
                stats.peakCompletionBatch = m_impl->m_peakCompletionBatch;
                stats.datagramReceiveCompletions = m_impl->m_datagramReceiveCompletions;
                stats.datagramSendCompletions = m_impl->m_datagramSendCompletions;
                stats.receivedDatagrams = m_impl->m_receivedDatagrams;
                stats.sentDatagrams = m_impl->m_sentDatagrams;
                stats.zeroLengthDatagrams = m_impl->m_zeroLengthDatagrams;
                stats.truncatedDatagrams = m_impl->m_truncatedDatagrams;
                stats.pendingDatagramSends = m_impl->m_pendingDatagramSends;
                return stats;
            }

            const char* IocpPoller::BackendName() const noexcept {
                return "iocp-overlapped";
            }

            void IocpPoller::PostTestPacket() noexcept {
                if (m_impl == nullptr || !m_impl->m_activated) return;
                if (!::PostQueuedCompletionStatus(
                    m_impl->m_completionPort,
                    0,
                    0,
                    nullptr)) {
                    SetLastError(static_cast<int>(::GetLastError()));
                }
            }

            std::uint64_t IocpPoller::ChannelRegistrationId(
                const Channel* channel) const noexcept {
                if (m_impl == nullptr || channel == nullptr) return 0;
                const auto found = m_impl->m_channelRegistrationIds.find(
                    const_cast<Channel*>(channel));
                return found == m_impl->m_channelRegistrationIds.end()
                    ? 0
                    : found->second;
            }

            void IocpPoller::PostTestChannelPacket(
                std::uint64_t registrationId) noexcept {
                if (m_impl == nullptr || !m_impl->m_activated || registrationId == 0) {
                    return;
                }
                try {
                    (void)::PostQueuedCompletionStatus(
                        m_impl->m_completionPort,
                        0,
                        EncodeChannelKey(registrationId),
                        nullptr);
                }
                catch (...) {
                    SetLastError(ERROR_ARITHMETIC_OVERFLOW);
                }
            }
        }
    }
}
