#include "net/platform/linux/EpollDriver.hpp"
#include "net/DatagramCompletionPolicy.hpp"
#include "net/PollerAccess.hpp"
#include "net/platform/SocketOps.hpp"

#include <LikesProgram/Net/Address.hpp>
#include <LikesProgram/Net/Buffer.hpp>
#include <LikesProgram/Net/BufferChain.hpp>
#include <LikesProgram/Net/Connection.hpp>
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdint>
#include <deque>
#include <limits>
#include <netinet/in.h>
#include <queue>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>

namespace {
    // 把公开 Channel 关注集合收敛为 level-triggered epoll flags。
    std::uint32_t ToEpollEvents(LikesProgram::Net::IOEvent events) noexcept {
        std::uint32_t result = EPOLLERR | EPOLLHUP | EPOLLRDHUP; // 始终观察错误与半关闭
        if (LikesProgram::Net::HasEvent(events, LikesProgram::Net::IOEvent::Read)) {
            result |= EPOLLIN | EPOLLPRI;
        }
        if (LikesProgram::Net::HasEvent(events, LikesProgram::Net::IOEvent::Write)) {
            result |= EPOLLOUT;
        }
        return result;
    }

    // 把本轮 epoll flags 转回 EventLoop 已有 IOEvent 契约。
    LikesProgram::Net::IOEvent ToChannelEvents(std::uint32_t events) noexcept {
        using LikesProgram::Net::IOEvent;
        IOEvent result = IOEvent::None; // 本轮只包含内核实际返回的事件位
        if ((events & (EPOLLIN | EPOLLPRI)) != 0) result |= IOEvent::Read;
        if ((events & EPOLLOUT) != 0) result |= IOEvent::Write;
        if ((events & EPOLLERR) != 0) result |= IOEvent::Error;
        if ((events & (EPOLLHUP | EPOLLRDHUP)) != 0) result |= IOEvent::Close;
        return result;
    }
}

namespace LikesProgram {
    namespace Net {
        namespace Internal {
            enum class EpollRegistrationKind {
                Channel,
                Connect,
                Accept,
                Connection
            };

            struct EpollRegistration {
                std::uint64_t m_id = 0; // 内核事件携带的唯一身份
                EpollRegistrationKind m_kind = EpollRegistrationKind::Channel; // 后续状态表分派类型
                SocketType m_fd = kInvalidSocket; // 只用于 epoll_ctl，不作为身份
                Channel* m_channel = nullptr; // Channel registration 的非拥有观察指针
                Poller::ConnectId m_operationId = Poller::InvalidConnectId; // connect 状态定位键
                Connection* m_connectionKey = nullptr; // Connection 状态定位键
            };

            struct EpollConnectState {
                Poller::ConnectId m_id = Poller::InvalidConnectId; // 调用方取消使用的稳定 id
                std::uint64_t m_registrationId = 0; // EINPROGRESS readiness 的内核身份
                SocketType m_fd = kInvalidSocket; // 成功 callback 前由 Poller 独占
                Poller::ConnectCallback m_callback; // issuer 线程 completion 入口
                bool m_readyToComplete = false; // 立即结果等待 Flush 延迟分派
                int m_error = 0; // 立即失败或 SO_ERROR 结果
            };

            struct EpollAcceptState {
                SocketType m_fd = kInvalidSocket; // listener 仅观察，不拥有生命周期
                std::uint64_t m_registrationId = 0; // listener readiness 的唯一身份
                Poller::AcceptCallback m_callback; // 每个 accepted fd 的消费入口
            };

            struct EpollDatagramWriteState {
                EpollDatagramWriteState(
                    DatagramWriteTarget target,
                    const Address& peer,
                    Buffer&& buffer,
                    std::size_t queueCost)
                    : m_target(target),
                    m_peer(peer),
                    m_buffer(std::move(buffer)),
                    m_queueCost(queueCost) {
                }

                DatagramWriteTarget m_target = DatagramWriteTarget::Invalid; // connected send 或显式 peer
                Address m_peer; // DTLS 完成回调和 sendto 使用的原始 peer
                Buffer m_buffer{ 0 }; // 完成或关闭前独占完整数据报 payload
                std::size_t m_queueCost = 0; // 空包也占一个背压单位
            };

            struct EpollConnectionState {
                std::shared_ptr<Connection> m_connection; // registration 与 callback 期间保持对象存活
                std::uint64_t m_registrationId = 0; // fd 复用时不复用的内核身份
                SocketType m_fd = kInvalidSocket; // Connection 持有实际关闭责任
                bool m_datagram = false; // UDP 使用数据报边界，TCP 使用字节流
                bool m_readEnabled = true; // 业务背压当前读开关
                bool m_closing = false; // 关闭后不再续投 read/write
                bool m_writeFlushQueued = false; // 防止同一 state 重复进入 Flush 队列
                std::size_t m_maxDatagramBytes = 0; // Start 时固定的单包接收容量
                std::size_t m_pendingWriteBytes = 0; // 全部未发送 BufferChain 字节数
                BufferChain m_writeChain; // partial send 完成前保持全部段所有权
                std::deque<EpollDatagramWriteState> m_datagramWrites; // UDP/DTLS 完整数据报 FIFO
            };

            struct EpollWriteFlushItem {
                Connection* m_connectionKey = nullptr; // 只用于重新查表，不解引用
                std::uint64_t m_registrationId = 0; // 地址复用时拒绝 stale queue item
            };

            struct EpollTimeoutState {
                Poller::TimeoutId m_id = Poller::InvalidTimeoutId; // active generation 身份
                std::chrono::steady_clock::time_point m_deadline{}; // 单调绝对到期点
                Poller::TimeoutCallback m_callback; // 到期时在 issuer 线程执行一次
            };

            struct EpollTimeoutHeapNode {
                std::chrono::steady_clock::time_point m_deadline{}; // heap 排序主键
                Poller::TimeoutId m_id = Poller::InvalidTimeoutId; // 与 active generation 对账
            };

            struct EpollTimeoutHeapCompare {
                // 让 priority_queue 顶部保持最早 deadline 和最小 id。
                bool operator()(
                    const EpollTimeoutHeapNode& left,
                    const EpollTimeoutHeapNode& right) const noexcept {
                    if (left.m_deadline != right.m_deadline) {
                        return left.m_deadline > right.m_deadline;
                    }
                    return left.m_id > right.m_id;
                }
            };

            struct EpollDriverImpl {
                int m_epollFd = -1; // Activate 后由本对象独占的 epoll 实例
                bool m_activated = false; // issuer 线程资源是否完整
                std::uint64_t m_nextRegistrationId = 1; // 单调且不使用零值的 registration id
                Poller::ConnectId m_nextConnectId = 1; // 单调且不使用零值的 connect id
                Poller::TimeoutId m_nextTimeoutId = 1; // 单调且不使用零值的 timer id
                std::unordered_map<std::uint64_t, EpollRegistration> m_registrations; // id 到稳定快照
                std::unordered_map<Channel*, std::uint64_t> m_channelRegistrationIds; // Channel 反向索引
                std::unordered_map<Poller::ConnectId, EpollConnectState> m_connectStates; // Poller-owned sockets
                std::unordered_map<SocketType, EpollAcceptState> m_acceptStates; // listener completion 状态
                std::unordered_map<Connection*, EpollConnectionState> m_connectionStates; // TCP/UDP completion 状态
                std::deque<EpollWriteFlushItem> m_writeFlushQueue; // QueueWrite 到 Flush 的 issuer 队列
                std::unordered_map<Poller::TimeoutId, EpollTimeoutState> m_timeouts; // 当前 active timer generation
                std::priority_queue<
                    EpollTimeoutHeapNode,
                    std::vector<EpollTimeoutHeapNode>,
                    EpollTimeoutHeapCompare> m_timeoutHeap; // 允许保留已取消 stale node
                std::array<epoll_event, 256> m_events{}; // 单轮有界 completion 批次
                std::uint64_t m_completedOperations = 0; // 已转换的 Channel/timer completion 总数
                std::uint64_t m_completionBatchCount = 0; // 至少包含一项 completion 的 Poll 数
                std::uint64_t m_completionBatchItems = 0; // 全部 Poll completion 数量之和
                std::uint64_t m_peakCompletionBatch = 0; // 单轮 completion 数量峰值
                std::uint64_t m_connectSubmissions = 0; // 已接受的 connect operation 数
                std::uint64_t m_connectCancelSubmissions = 0; // 已取消的 connect operation 数
                std::uint64_t m_acceptSubmissions = 0; // 已登记的 listener operation 数
                std::uint64_t m_readSubmissions = 0; // 启用 TCP read interest 的次数
                std::uint64_t m_receivedBytes = 0; // TCP read completion 总字节数
                std::uint64_t m_receiveCompletions = 0; // 成功 owning Buffer read 次数
                std::uint64_t m_datagramReceiveCompletions = 0; // 成功 recvfrom 次数
                std::uint64_t m_datagramSendCompletions = 0; // 完整 send/sendto 次数
                std::uint64_t m_receivedDatagrams = 0; // 已交付业务的数据报数
                std::uint64_t m_sentDatagrams = 0; // 已完整发送的数据报数
                std::uint64_t m_zeroLengthDatagrams = 0; // 收发空数据报总数
                std::uint64_t m_truncatedDatagrams = 0; // 接收容量不足的数据报数
                std::size_t m_pendingDatagramSends = 0; // 当前 FIFO 内数据报节点数
                std::uint64_t m_datagramSendBatchSubmissions = 0; // epoll 单数据报 syscall 数
                std::size_t m_maximumDatagramSendBatch = 0; // epoll 有效值固定为 1

                // 分配不与活动映射冲突的非零 registration id。
                std::uint64_t NextRegistrationId() noexcept {
                    std::uint64_t registrationId = m_nextRegistrationId++;
                    while (registrationId == 0
                        || m_registrations.find(registrationId) != m_registrations.end()) {
                        registrationId = m_nextRegistrationId++;
                    }
                    return registrationId;
                }

                // 分配不与活动状态冲突的非零 connect id。
                Poller::ConnectId NextConnectId() noexcept {
                    Poller::ConnectId connectId = m_nextConnectId++;
                    while (connectId == Poller::InvalidConnectId
                        || m_connectStates.find(connectId) != m_connectStates.end()) {
                        connectId = m_nextConnectId++;
                    }
                    return connectId;
                }

                // 分配不与 active generation 冲突的非零 timer id。
                Poller::TimeoutId NextTimeoutId() noexcept {
                    Poller::TimeoutId timeoutId = m_nextTimeoutId++;
                    while (timeoutId == Poller::InvalidTimeoutId
                        || m_timeouts.find(timeoutId) != m_timeouts.end()) {
                        timeoutId = m_nextTimeoutId++;
                    }
                    return timeoutId;
                }

                // 丢弃已取消或不再匹配 active generation 的 heap node。
                void PruneTimeoutHeap() noexcept {
                    while (!m_timeoutHeap.empty()) {
                        const EpollTimeoutHeapNode& node = m_timeoutHeap.top(); // 当前最早候选
                        const auto found = m_timeouts.find(node.m_id);
                        if (found != m_timeouts.end()
                            && found->second.m_deadline == node.m_deadline) {
                            return;
                        }
                        m_timeoutHeap.pop();
                    }
                }

                // 把绝对 deadline 转成 epoll_wait 向上取整的毫秒预算。
                int WaitMillisecondsUntil(
                    std::chrono::steady_clock::time_point deadline) const noexcept {
                    const auto now = std::chrono::steady_clock::now(); // 每次 EINTR 后重新读取单调时钟
                    if (deadline <= now) return 0;

                    const auto remaining = deadline - now; // 共享原始绝对 deadline 的剩余预算
                    const auto rounded = std::chrono::ceil<std::chrono::milliseconds>(remaining);
                    if (rounded.count() >= INT_MAX) return INT_MAX;
                    return static_cast<int>(rounded.count());
                }

                // 记录一个至少包含一项 completion 的 issuer 批次。
                void RecordCompletionBatch(std::uint64_t batchItems) noexcept {
                    if (batchItems == 0) return;
                    ++m_completionBatchCount;
                    m_completionBatchItems += batchItems;
                    m_peakCompletionBatch = std::max(m_peakCompletionBatch, batchItems);
                }
            };

            EpollDriver::EpollDriver(EventLoop* ownerLoop)
                : Poller(ownerLoop), m_impl(new EpollDriverImpl{}) {
            }

            EpollDriver::~EpollDriver() {
                if (m_impl != nullptr) {
                    for (auto& item : m_impl->m_connectionStates) {
                        if (item.second.m_connection) {
                            PollerAccess::Detach(*item.second.m_connection); // Poller 先于 Connection 失效
                        }
                    }
                    m_impl->m_connectionStates.clear();
                    m_impl->m_writeFlushQueue.clear();
                    for (auto& item : m_impl->m_connectStates) {
                        Internal::CloseSocket(item.second.m_fd); // 未完成 connect socket 仍由 Poller 独占
                        item.second.m_fd = kInvalidSocket;
                    }
                    m_impl->m_connectStates.clear();
                    m_impl->m_acceptStates.clear();
                    m_impl->m_registrations.clear();
                    m_impl->m_channelRegistrationIds.clear();
                    m_impl->m_timeouts.clear();
                }
                if (m_impl != nullptr && m_impl->m_epollFd >= 0) {
                    (void)::close(m_impl->m_epollFd);
                    m_impl->m_epollFd = -1;
                }
                delete m_impl;
                m_impl = nullptr;
            }

            void EpollDriver::RemoveOperationRegistration(
                std::uint64_t registrationId) noexcept {
                if (m_impl == nullptr || registrationId == 0) return;
                const auto found = m_impl->m_registrations.find(registrationId);
                if (found == m_impl->m_registrations.end()) return;

                const SocketType fd = found->second.m_fd; // erase 前固定 epoll_ctl 目标
                m_impl->m_registrations.erase(found); // 迟到事件从这一刻开始只会被忽略
                if (m_impl->m_epollFd < 0
                    || ::epoll_ctl(m_impl->m_epollFd, EPOLL_CTL_DEL, fd, nullptr) == 0) {
                    return;
                }
                const int error = errno; // 已关闭或已删除等价于 registration 已撤销
                if (error != ENOENT && error != EBADF) SetLastError(error);
            }

            void EpollDriver::CompleteConnect(ConnectId connectId, int error) noexcept {
                if (m_impl == nullptr || connectId == InvalidConnectId) return;
                const auto found = m_impl->m_connectStates.find(connectId);
                if (found == m_impl->m_connectStates.end()) return;

                const std::uint64_t registrationId = found->second.m_registrationId; // callback 前撤销内核身份
                SocketType completedFd = found->second.m_fd; // 成功时把唯一关闭责任移交 callback
                ConnectCallback callback = std::move(found->second.m_callback); // erase 后仍可安全调用
                RemoveOperationRegistration(registrationId);
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
                    // 用户成功回调异常时收回已移交 socket，失败路径已提前关闭。
                    if (error == 0) Internal::CloseSocket(completedFd);
                }
            }

            std::uint64_t EpollDriver::DrainAccept(
                SocketType listenFd,
                std::uint64_t registrationId) noexcept {
                if (m_impl == nullptr || listenFd == kInvalidSocket || registrationId == 0) return 0;

                std::uint64_t completed = 0; // 当前 readiness 实际 drain 的 accepted socket 数
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
                    if (clientFd >= 0) {
                        AcceptCallback callback; // callback 可能 StopAccept 并删除当前 state
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
                            // 用户 accept 消费入口失败时关闭 fd，避免 completion 路径泄漏。
                            Internal::CloseSocket(clientFd);
                        }
                        ++completed;
                        ++m_impl->m_completedOperations;
                        continue;
                    }

                    const int error = Internal::GetLastSocketError(); // accept4 失败的稳定 errno
                    if (Internal::IsInterrupted(error)) continue;
                    if (Internal::IsWouldBlock(error)) break;
                    if (error != EMFILE && error != ENFILE && error != ENOBUFS && error != ENOMEM) {
                        SetLastError(error);
                    }
                    break;
                }
                return completed;
            }

            bool EpollDriver::UpdateConnectionInterest(Connection* connection) noexcept {
                if (m_impl == nullptr || connection == nullptr) return false;
                const auto found = m_impl->m_connectionStates.find(connection);
                if (found == m_impl->m_connectionStates.end() || found->second.m_closing) return false;

                EpollConnectionState& state = found->second; // issuer 线程独占 interest 快照
                epoll_event event{};
                event.events = EPOLLERR | EPOLLHUP | EPOLLRDHUP;
                if (state.m_readEnabled) event.events |= EPOLLIN | EPOLLPRI;
                const bool writePending = state.m_datagram
                    ? !state.m_datagramWrites.empty()
                    : !state.m_writeChain.Empty();
                if (writePending) event.events |= EPOLLOUT;
                event.data.u64 = state.m_registrationId;
                if (::epoll_ctl(m_impl->m_epollFd, EPOLL_CTL_MOD, state.m_fd, &event) == 0) {
                    return true;
                }
                SetLastError(errno);
                return false;
            }

            std::uint64_t EpollDriver::DrainConnectionRead(
                Connection* connection,
                std::uint64_t registrationId,
                bool drainForClose) noexcept {
                if (m_impl == nullptr || connection == nullptr || registrationId == 0) return 0;

                constexpr std::size_t kReadCapacity = 64 * 1024; // 单次 owning Buffer 接收上界
                constexpr int kMaximumDrainIterations = 64; // 避免单连接长期占用 issuer
                std::uint64_t completed = 0; // 本轮 read/EOF/error logical completion 数
                for (int iteration = 0; iteration < kMaximumDrainIterations; ++iteration) {
                    const auto found = m_impl->m_connectionStates.find(connection);
                    if (found == m_impl->m_connectionStates.end()
                        || found->second.m_registrationId != registrationId
                        || found->second.m_closing
                        || (!found->second.m_readEnabled && !drainForClose)) {
                        break;
                    }

                    const SocketType fd = found->second.m_fd; // callback 前固定本轮系统调用输入
                    std::shared_ptr<Connection> snapshot = found->second.m_connection; // 回调期间保持对象存活
                    try {
                        Buffer input(0); // epoll 直接产生拥有型 completion Buffer
                        std::uint8_t* destination = input.PrepareWrite(kReadCapacity); // socket 直接写入连续区
                        const std::int64_t received = Internal::ReceiveSocket(
                            fd,
                            destination,
                            kReadCapacity,
                            0); // recv 的 errno 风格结果
                        if (received > 0) {
                            const std::size_t receivedBytes = static_cast<std::size_t>(received); // 本次完整 TCP 字节数
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
                            PollerAccess::PeerClosed(*snapshot); // 已先 drain 前序 payload，再发布 EOF
                            break;
                        }

                        const int error = Internal::GetLastSocketError(); // recv 失败的稳定 errno
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

            std::uint64_t EpollDriver::DrainConnectionWrite(
                Connection* connection,
                std::uint64_t registrationId) noexcept {
                if (m_impl == nullptr || connection == nullptr || registrationId == 0) return 0;

                constexpr int kMaximumDrainIterations = 64; // partial send 公平上界
                std::uint64_t completed = 0; // 本轮成功 send 与错误 completion 数
                for (int iteration = 0; iteration < kMaximumDrainIterations; ++iteration) {
                    const auto found = m_impl->m_connectionStates.find(connection);
                    if (found == m_impl->m_connectionStates.end()
                        || found->second.m_registrationId != registrationId
                        || found->second.m_closing
                        || found->second.m_writeChain.Empty()) {
                        break;
                    }

                    EpollConnectionState& state = found->second; // 系统调用返回前不执行用户 callback
                    const BufferSlice segment = state.m_writeChain.Segment(0); // 当前 FIFO 头借用视图
                    const std::int64_t sent = Internal::SendSocket(
                        state.m_fd,
                        segment.Data(),
                        segment.Size(),
                        0);
                    if (sent > 0) {
                        const std::size_t sentBytes = static_cast<std::size_t>(sent); // 本次实际消费字节数
                        state.m_writeChain.Consume(sentBytes);
                        state.m_pendingWriteBytes -= std::min(sentBytes, state.m_pendingWriteBytes);
                        const std::size_t pendingBytes = state.m_pendingWriteBytes; // callback 使用剩余量快照
                        std::shared_ptr<Connection> snapshot = state.m_connection; // Low/close 期间保持对象存活
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
                            PollerAccess::WriteComplete(*snapshot); // interest 收窄后再发布排空语义
                            break;
                        }
                        continue;
                    }

                    const int error = sent == 0 ? EIO : Internal::GetLastSocketError(); // 零进度视为写错误
                    if (sent < 0 && Internal::IsInterrupted(error)) continue;
                    if (sent < 0 && Internal::IsWouldBlock(error)) {
                        if (!UpdateConnectionInterest(connection)) {
                            std::shared_ptr<Connection> snapshot = state.m_connection; // epoll_ctl 失败通知对象
                            PollerAccess::Error(*snapshot, EIO);
                        }
                        break;
                    }
                    std::shared_ptr<Connection> snapshot = state.m_connection; // 错误回调会删除当前 state
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
                    std::shared_ptr<Connection> snapshot = remaining->second.m_connection; // 公平上界后保留写通知
                    if (!UpdateConnectionInterest(connection)) PollerAccess::Error(*snapshot, EIO);
                }
                return completed;
            }

            std::uint64_t EpollDriver::DrainDatagramRead(
                Connection* connection,
                std::uint64_t registrationId) noexcept {
                if (m_impl == nullptr || connection == nullptr || registrationId == 0) return 0;

                constexpr int kMaximumDrainIterations = 64; // 保持边界同时限制单连接占用
                std::uint64_t completed = 0; // 本轮成功数据报或错误 completion 数
                for (int iteration = 0; iteration < kMaximumDrainIterations; ++iteration) {
                    const auto found = m_impl->m_connectionStates.find(connection);
                    if (found == m_impl->m_connectionStates.end()
                        || found->second.m_registrationId != registrationId
                        || !found->second.m_datagram
                        || found->second.m_closing
                        || !found->second.m_readEnabled) {
                        break;
                    }

                    const SocketType fd = found->second.m_fd; // callback 前固定系统调用输入
                    const std::size_t capacity = found->second.m_maxDatagramBytes; // Start 时读取一次
                    std::shared_ptr<Connection> snapshot = found->second.m_connection; // callback 保活
                    try {
                        Buffer input(0); // epoll 数据报使用普通 owning Buffer
                        std::uint8_t* destination = input.PrepareWrite(capacity);
                        sockaddr_storage peerStorage{};
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
                                false); // recvfrom(MSG_TRUNC) 以返回长度表达截断
                            input.HasWritten(result.payloadBytes);
                            Address peer(peerStorage, peerLength);
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

                        const int error = Internal::GetLastSocketError(); // recvfrom 失败的稳定 errno
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

            std::uint64_t EpollDriver::DrainDatagramWrite(
                Connection* connection,
                std::uint64_t registrationId) noexcept {
                if (m_impl == nullptr || connection == nullptr || registrationId == 0) return 0;

                constexpr int kMaximumDrainIterations = 16; // 给关闭任务留下可观察的 FIFO 尾部
                std::uint64_t completed = 0; // 本轮完整 send 或首个 fatal error 数
                for (int iteration = 0; iteration < kMaximumDrainIterations; ++iteration) {
                    const auto found = m_impl->m_connectionStates.find(connection);
                    if (found == m_impl->m_connectionStates.end()
                        || found->second.m_registrationId != registrationId
                        || !found->second.m_datagram
                        || found->second.m_closing
                        || found->second.m_datagramWrites.empty()) {
                        break;
                    }

                    EpollConnectionState& state = found->second; // syscall 返回前不执行 callback
                    EpollDatagramWriteState& head = state.m_datagramWrites.front();
                    const std::size_t payloadBytes = head.m_buffer.ReadableBytes();
                    ++m_impl->m_datagramSendBatchSubmissions; // epoll 每个 syscall 是一项 batch
                    m_impl->m_maximumDatagramSendBatch = 1;
                    std::int64_t sent = -1;
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
                        sent = -1;
                        errno = EDESTADDRREQ;
                    }

                    if (sent >= 0 && DatagramWriteCompleted(
                            static_cast<std::size_t>(sent), payloadBytes)) {
                        EpollDatagramWriteState completedWrite = std::move(head); // callback 前脱离 state
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

                    std::shared_ptr<Connection> snapshot = state.m_connection; // Error 前回收全部队列成本
                    std::deque<EpollDatagramWriteState> abandoned;
                    abandoned.swap(state.m_datagramWrites);
                    const std::size_t abandonedCount = abandoned.size();
                    state.m_pendingWriteBytes = 0;
                    m_impl->m_pendingDatagramSends = abandonedCount >= m_impl->m_pendingDatagramSends
                        ? 0
                        : m_impl->m_pendingDatagramSends - abandonedCount;
                    for (const EpollDatagramWriteState& datagram : abandoned) {
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

            bool EpollDriver::Activate() {
                if (m_impl == nullptr) return false;
                if (m_impl->m_activated) return true;

                const int epollFd = ::epoll_create1(EPOLL_CLOEXEC); // issuer 线程创建唯一 epoll 实例
                if (epollFd < 0) {
                    SetLastError(errno);
                    return false;
                }
                m_impl->m_epollFd = epollFd;
                m_impl->m_activated = true;
                return true;
            }

            bool EpollDriver::AddChannel(Channel* channel) {
                if (m_impl == nullptr || !m_impl->m_activated
                    || channel == nullptr || channel->GetSocket() == kInvalidSocket) {
                    return false;
                }
                if (m_impl->m_channelRegistrationIds.find(channel)
                    != m_impl->m_channelRegistrationIds.end()) {
                    return UpdateChannel(channel);
                }
                if (!StoreChannel(channel)) return false;

                const std::uint64_t registrationId = m_impl->NextRegistrationId(); // fd 复用时不复用身份
                EpollRegistration registration{}; // 映射在 epoll_ctl 前完整建立，失败时统一回滚
                registration.m_id = registrationId;
                registration.m_kind = EpollRegistrationKind::Channel;
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

                epoll_event event{}; // 内核只保存 registration id，不保存 fd 或裸指针
                event.events = ToEpollEvents(channel->Events());
                event.data.u64 = registrationId;
                if (::epoll_ctl(m_impl->m_epollFd, EPOLL_CTL_ADD, registration.m_fd, &event) != 0) {
                    const int error = errno; // 回滚后仍保留原始系统错误
                    m_impl->m_channelRegistrationIds.erase(channel);
                    m_impl->m_registrations.erase(registrationId);
                    (void)EraseChannel(channel);
                    SetLastError(error);
                    return false;
                }

                channel->SetIndex(Channel::Index::Added);
                return true;
            }

            bool EpollDriver::RemoveChannel(Channel* channel) {
                if (m_impl == nullptr || channel == nullptr) return false;
                const auto foundId = m_impl->m_channelRegistrationIds.find(channel);
                if (foundId == m_impl->m_channelRegistrationIds.end()) return false;

                const std::uint64_t registrationId = foundId->second; // 先撤销身份，迟到事件随后只会被忽略
                const auto foundRegistration = m_impl->m_registrations.find(registrationId);
                const SocketType fd = foundRegistration == m_impl->m_registrations.end()
                    ? channel->GetSocket()
                    : foundRegistration->second.m_fd;
                m_impl->m_channelRegistrationIds.erase(foundId);
                m_impl->m_registrations.erase(registrationId);
                (void)EraseChannel(channel);
                channel->SetIndex(Channel::Index::Deleted);

                if (m_impl->m_epollFd < 0
                    || ::epoll_ctl(m_impl->m_epollFd, EPOLL_CTL_DEL, fd, nullptr) == 0) {
                    return true;
                }
                const int error = errno; // 已关闭或已删除等价于 registration 已撤销
                if (error == ENOENT || error == EBADF) return true;
                SetLastError(error);
                return false;
            }

            bool EpollDriver::UpdateChannel(Channel* channel) {
                if (m_impl == nullptr || !m_impl->m_activated || channel == nullptr) return false;
                const auto foundId = m_impl->m_channelRegistrationIds.find(channel);
                if (foundId == m_impl->m_channelRegistrationIds.end()) return AddChannel(channel);

                const auto foundRegistration = m_impl->m_registrations.find(foundId->second);
                if (foundRegistration == m_impl->m_registrations.end()) return false;
                epoll_event event{}; // MOD 保持原 registration id，不制造身份切换窗口
                event.events = ToEpollEvents(channel->Events());
                event.data.u64 = foundId->second;
                if (::epoll_ctl(
                    m_impl->m_epollFd,
                    EPOLL_CTL_MOD,
                    foundRegistration->second.m_fd,
                    &event) != 0) {
                    SetLastError(errno);
                    return false;
                }
                return true;
            }

            void EpollDriver::Poll(int timeoutMs, std::vector<Channel*>& active) {
                active.clear();
                if (m_impl == nullptr || !m_impl->m_activated) return;

                const std::uint64_t completedBeforeFlush = m_impl->m_completedOperations; // 立即 connect 完成基线
                Flush();
                const bool flushedCompletion = m_impl->m_completedOperations != completedBeforeFlush;
                const auto callerDeadline = std::chrono::steady_clock::now()
                    + std::chrono::milliseconds(flushedCompletion ? 0 : std::max(timeoutMs, 0)); // 完成后立即归还 EventLoop
                m_impl->PruneTimeoutHeap();
                auto waitDeadline = callerDeadline; // timer 只能收窄本轮调用方预算
                if (!m_impl->m_timeoutHeap.empty()) {
                    waitDeadline = std::min(waitDeadline, m_impl->m_timeoutHeap.top().m_deadline);
                }

                int eventCount = 0; // EINTR 到达时不重置 waitDeadline
                while (true) {
                    const int waitMilliseconds = m_impl->WaitMillisecondsUntil(waitDeadline); // 当前剩余预算
                    eventCount = ::epoll_wait(
                        m_impl->m_epollFd,
                        m_impl->m_events.data(),
                        static_cast<int>(m_impl->m_events.size()),
                        waitMilliseconds);
                    if (eventCount >= 0) break;
                    if (errno == EINTR) {
                        if (std::chrono::steady_clock::now() >= waitDeadline) {
                            eventCount = 0;
                            break;
                        }
                        continue;
                    }
                    SetLastError(errno);
                    eventCount = 0;
                    break;
                }

                std::uint64_t batchItems = 0; // 本轮实际映射的 Channel 与 timer completion 数
                for (int index = 0; index < eventCount; ++index) {
                    const epoll_event& event = m_impl->m_events[static_cast<std::size_t>(index)];
                    const auto found = m_impl->m_registrations.find(event.data.u64);
                    if (found == m_impl->m_registrations.end()) continue;

                    const EpollRegistration registration = found->second; // callback 前使用稳定身份快照
                    if (registration.m_kind == EpollRegistrationKind::Channel) {
                        if (registration.m_channel == nullptr) continue;
                        registration.m_channel->SetRevents(ToChannelEvents(event.events));
                        active.push_back(registration.m_channel);
                        ++m_impl->m_completedOperations;
                        ++batchItems;
                        continue;
                    }
                    if (registration.m_kind == EpollRegistrationKind::Connect) {
                        const auto connect = m_impl->m_connectStates.find(registration.m_operationId);
                        if (connect == m_impl->m_connectStates.end()
                            || connect->second.m_registrationId != registration.m_id) {
                            continue;
                        }
                        const int error = Internal::GetSocketPendingError(connect->second.m_fd); // SO_ERROR 完成结果
                        CompleteConnect(registration.m_operationId, error);
                        ++batchItems;
                        continue;
                    }
                    if (registration.m_kind == EpollRegistrationKind::Accept) {
                        batchItems += DrainAccept(registration.m_fd, registration.m_id);
                        continue;
                    }
                    if (registration.m_kind == EpollRegistrationKind::Connection) {
                        const auto connection = m_impl->m_connectionStates.find(
                            registration.m_connectionKey);
                        if (connection == m_impl->m_connectionStates.end()
                            || connection->second.m_registrationId != registration.m_id) {
                            continue;
                        }

                        const bool closeReady = (event.events & (EPOLLHUP | EPOLLRDHUP)) != 0; // 最后 payload 也需 drain
                        if ((event.events & (EPOLLIN | EPOLLPRI)) != 0 || closeReady) {
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
                        if ((event.events & EPOLLERR) != 0) {
                            int error = Internal::GetSocketPendingError(afterRead->second.m_fd); // EPOLLERR 的 errno
                            if (error == 0) error = EIO;
                            std::shared_ptr<Connection> snapshot = afterRead->second.m_connection; // Error 会删除 state
                            ++m_impl->m_completedOperations;
                            ++batchItems;
                            PollerAccess::Error(*snapshot, error);
                            continue;
                        }
                        if ((event.events & EPOLLOUT) != 0) {
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

                while (true) {
                    m_impl->PruneTimeoutHeap();
                    if (m_impl->m_timeoutHeap.empty()
                        || m_impl->m_timeoutHeap.top().m_deadline
                            > std::chrono::steady_clock::now()) {
                        break;
                    }

                    const EpollTimeoutHeapNode node = m_impl->m_timeoutHeap.top(); // 到期 generation 快照
                    m_impl->m_timeoutHeap.pop();
                    const auto found = m_impl->m_timeouts.find(node.m_id);
                    if (found == m_impl->m_timeouts.end()
                        || found->second.m_deadline != node.m_deadline) {
                        continue;
                    }

                    TimeoutCallback callback = std::move(found->second.m_callback); // 先撤销 active 再执行用户逻辑
                    m_impl->m_timeouts.erase(found);
                    ++batchItems;
                    ++m_impl->m_completedOperations;
                    if (callback) callback();
                }

                m_impl->RecordCompletionBatch(batchItems);
            }

            void EpollDriver::Flush() {
                if (m_impl == nullptr || !m_impl->m_activated) return;

                std::uint64_t completed = 0; // 本轮延迟 connect 与同步 write completion 数
                while (true) {
                    ConnectId readyId = InvalidConnectId; // 每次 callback 后重新遍历，允许 reentrant map 修改
                    int readyError = 0; // 当前立即 connect 的同步系统调用结果
                    for (const auto& item : m_impl->m_connectStates) {
                        if (!item.second.m_readyToComplete) continue;
                        readyId = item.first;
                        readyError = item.second.m_error;
                        break;
                    }
                    if (readyId == InvalidConnectId) break;
                    CompleteConnect(readyId, readyError);
                    ++completed;
                }

                while (!m_impl->m_writeFlushQueue.empty()) {
                    const EpollWriteFlushItem item = m_impl->m_writeFlushQueue.front(); // pop 后按 generation 重查
                    m_impl->m_writeFlushQueue.pop_front();
                    const auto found = m_impl->m_connectionStates.find(item.m_connectionKey);
                    if (found == m_impl->m_connectionStates.end()
                        || found->second.m_registrationId != item.m_registrationId) {
                        continue;
                    }
                    found->second.m_writeFlushQueued = false;
                    completed += found->second.m_datagram
                        ? DrainDatagramWrite(
                            item.m_connectionKey,
                            item.m_registrationId)
                        : DrainConnectionWrite(
                            item.m_connectionKey,
                            item.m_registrationId);
                }
                m_impl->RecordCompletionBatch(completed);
            }

            Poller::ConnectId EpollDriver::StartConnect(
                const Address& remoteAddress,
                ConnectCallback callback) noexcept {
                if (m_impl == nullptr || !m_impl->m_activated
                    || !remoteAddress.IsValid() || !callback) return InvalidConnectId;

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

                const ConnectId connectId = m_impl->NextConnectId(); // 调用方取消使用的稳定 operation id
                EpollConnectState state{}; // connect 系统调用结果只写入已接受的 state
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

                EpollConnectState& stored = m_impl->m_connectStates.at(connectId); // emplace 后地址稳定到完成
                const int connectResult = Internal::ConnectSocket(
                    fd,
                    remoteAddress.SockAddr(),
                    remoteAddress.Length());
                if (connectResult == 0) {
                    stored.m_readyToComplete = true;
                    stored.m_error = 0;
                }
                else {
                    const int error = Internal::GetLastSocketError(); // 非阻塞 connect 的初始结果
                    if (error == EINPROGRESS) {
                        const std::uint64_t registrationId = m_impl->NextRegistrationId(); // fd 复用不复用身份
                        EpollRegistration registration{};
                        registration.m_id = registrationId;
                        registration.m_kind = EpollRegistrationKind::Connect;
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

                        epoll_event event{}; // connect readiness 只携带稳定 registration id
                        event.events = EPOLLOUT | EPOLLERR | EPOLLHUP;
                        event.data.u64 = registrationId;
                        if (::epoll_ctl(m_impl->m_epollFd, EPOLL_CTL_ADD, fd, &event) != 0) {
                            const int registrationError = errno; // 回滚后保留 epoll_ctl 错误
                            m_impl->m_registrations.erase(registrationId);
                            m_impl->m_connectStates.erase(connectId);
                            Internal::CloseSocket(fd);
                            SetLastError(registrationError);
                            return InvalidConnectId;
                        }
                        stored.m_registrationId = registrationId;
                    }
                    else {
                        stored.m_readyToComplete = true; // 同步失败也延迟到 Flush/Poll callback
                        stored.m_error = error;
                    }
                }

                ++m_impl->m_connectSubmissions;
                return connectId;
            }

            void EpollDriver::CancelConnect(ConnectId connectId) noexcept {
                if (m_impl == nullptr || connectId == InvalidConnectId) return;
                const auto found = m_impl->m_connectStates.find(connectId);
                if (found == m_impl->m_connectStates.end()) return;

                const std::uint64_t registrationId = found->second.m_registrationId; // 关闭前撤销内核身份
                const SocketType fd = found->second.m_fd; // state erase 后仍需关闭的唯一 socket
                RemoveOperationRegistration(registrationId);
                m_impl->m_connectStates.erase(found);
                Internal::CloseSocket(fd);
                ++m_impl->m_connectCancelSubmissions;
            }

            bool EpollDriver::StartConnection(const std::shared_ptr<Connection>& connection) {
                if (m_impl == nullptr || !m_impl->m_activated || !connection
                    || connection->GetSocket() == kInvalidSocket
                    || (connection->GetTransportKind() != TransportKind::Tcp
                        && connection->GetTransportKind() != TransportKind::Udp)) {
                    return false;
                }
                if (m_impl->m_connectionStates.find(connection.get())
                    != m_impl->m_connectionStates.end()) return true;

                const std::uint64_t registrationId = m_impl->NextRegistrationId(); // Connection 地址不是内核身份
                EpollRegistration registration{};
                registration.m_id = registrationId;
                registration.m_kind = EpollRegistrationKind::Connection;
                registration.m_fd = connection->GetSocket();
                registration.m_connectionKey = connection.get();
                try {
                    EpollConnectionState state{}; // map value 持有 shared_ptr 与 BufferChain 所有权
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
                epoll_event event{}; // TCP/UDP 从第一轮起直接接收 owning completion
                event.events = EPOLLERR | EPOLLHUP | EPOLLRDHUP;
                if (found->second.m_readEnabled) event.events |= EPOLLIN | EPOLLPRI;
                event.data.u64 = registrationId;
                if (::epoll_ctl(
                    m_impl->m_epollFd,
                    EPOLL_CTL_ADD,
                    connection->GetSocket(),
                    &event) != 0) {
                    const int error = errno; // Start 失败时不接管 Connection socket 关闭责任
                    m_impl->m_connectionStates.erase(connection.get());
                    m_impl->m_registrations.erase(registrationId);
                    SetLastError(error);
                    return false;
                }
                if (found->second.m_readEnabled) ++m_impl->m_readSubmissions;
                return true;
            }

            bool EpollDriver::StartAccept(SocketType listenFd, AcceptCallback callback) {
                if (m_impl == nullptr || !m_impl->m_activated
                    || listenFd == kInvalidSocket || !callback) return false;
                if (m_impl->m_acceptStates.find(listenFd) != m_impl->m_acceptStates.end()) return true;

                const std::uint64_t registrationId = m_impl->NextRegistrationId(); // listener fd 不作为身份
                EpollAcceptState state{};
                state.m_fd = listenFd;
                state.m_registrationId = registrationId;
                state.m_callback = std::move(callback);
                EpollRegistration registration{};
                registration.m_id = registrationId;
                registration.m_kind = EpollRegistrationKind::Accept;
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

                epoll_event event{}; // level-triggered listener 会在 Poll 内 drain 到 EAGAIN
                event.events = EPOLLIN | EPOLLERR | EPOLLHUP;
                event.data.u64 = registrationId;
                if (::epoll_ctl(m_impl->m_epollFd, EPOLL_CTL_ADD, listenFd, &event) != 0) {
                    const int error = errno; // registration 失败不接管 listener 生命周期
                    m_impl->m_acceptStates.erase(listenFd);
                    m_impl->m_registrations.erase(registrationId);
                    SetLastError(error);
                    return false;
                }
                ++m_impl->m_acceptSubmissions;
                return true;
            }

            void EpollDriver::StopAccept(SocketType listenFd) {
                if (m_impl == nullptr || listenFd == kInvalidSocket) return;
                const auto found = m_impl->m_acceptStates.find(listenFd);
                if (found == m_impl->m_acceptStates.end()) return;

                const std::uint64_t registrationId = found->second.m_registrationId; // erase 前固定 id
                RemoveOperationRegistration(registrationId);
                m_impl->m_acceptStates.erase(found);
            }

            Poller::TimeoutId EpollDriver::ScheduleTimeout(
                std::chrono::milliseconds delay,
                TimeoutCallback callback) noexcept {
                if (m_impl == nullptr || !m_impl->m_activated || !callback) return InvalidTimeoutId;

                const TimeoutId timeoutId = m_impl->NextTimeoutId(); // issuer 线程独占分配 generation
                EpollTimeoutState state{}; // active 表持有唯一 callback 所有权
                state.m_id = timeoutId;
                state.m_deadline = std::chrono::steady_clock::now() + delay;
                state.m_callback = std::move(callback);
                try {
                    m_impl->m_timeouts.emplace(timeoutId, std::move(state));
                    m_impl->m_timeoutHeap.push({ m_impl->m_timeouts.at(timeoutId).m_deadline, timeoutId });
                }
                catch (...) {
                    m_impl->m_timeouts.erase(timeoutId);
                    return InvalidTimeoutId;
                }
                return timeoutId;
            }

            void EpollDriver::CancelTimeout(TimeoutId timeoutId) noexcept {
                if (m_impl == nullptr || timeoutId == InvalidTimeoutId) return;
                m_impl->m_timeouts.erase(timeoutId); // heap node 在下一次 prune 时惰性丢弃
            }

            bool EpollDriver::QueueWrite(Connection* connection, Buffer&& buffer) {
                BufferChain chain; // 单 Buffer 入口统一进入多段写所有权链
                chain.Append(std::move(buffer));
                return QueueWrite(connection, std::move(chain));
            }

            bool EpollDriver::QueueWrite(Connection* connection, BufferChain&& chain) {
                if (m_impl == nullptr || connection == nullptr) return false;
                const auto found = m_impl->m_connectionStates.find(connection);
                if (found == m_impl->m_connectionStates.end()) return false;

                const std::size_t incomingBytes = chain.ReadableBytes(); // 移动前保存背压统计
                if (found->second.m_closing || incomingBytes == 0) return true;
                if (incomingBytes > (std::numeric_limits<std::size_t>::max)()
                    - found->second.m_pendingWriteBytes) {
                    std::shared_ptr<Connection> snapshot = found->second.m_connection; // 错误会移除 state
                    PollerAccess::Error(*snapshot, ENOBUFS);
                    return true;
                }

                try {
                    found->second.m_writeChain.Append(std::move(chain)); // 只移动 Buffer PImpl，不复制 payload
                }
                catch (...) {
                    std::shared_ptr<Connection> snapshot = found->second.m_connection; // 分配失败统一关闭
                    PollerAccess::Error(*snapshot, ENOMEM);
                    return true;
                }
                found->second.m_pendingWriteBytes += incomingBytes;
                const std::size_t pendingBytes = found->second.m_pendingWriteBytes; // callback 输入快照
                const std::uint64_t registrationId = found->second.m_registrationId; // relookup generation
                std::shared_ptr<Connection> snapshot = found->second.m_connection; // callback 期间保持对象
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

            bool EpollDriver::QueueDatagramWrite(
                Connection* connection,
                const Address& peer,
                Buffer&& buffer) {
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
                const std::uint64_t registrationId = found->second.m_registrationId;
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

            void EpollDriver::SetReadEnabled(Connection* connection, bool enabled) {
                if (m_impl == nullptr || connection == nullptr) return;
                const auto found = m_impl->m_connectionStates.find(connection);
                if (found == m_impl->m_connectionStates.end() || found->second.m_closing
                    || found->second.m_readEnabled == enabled) return;

                found->second.m_readEnabled = enabled;
                if (enabled) ++m_impl->m_readSubmissions;
                std::shared_ptr<Connection> snapshot = found->second.m_connection; // epoll_ctl 失败通知对象
                if (!UpdateConnectionInterest(connection)) PollerAccess::Error(*snapshot, EIO);
            }

            std::size_t EpollDriver::PendingWriteBytes(
                const Connection* connection) const noexcept {
                if (m_impl == nullptr || connection == nullptr) return 0;
                const auto found = m_impl->m_connectionStates.find(
                    const_cast<Connection*>(connection));
                return found == m_impl->m_connectionStates.end()
                    ? 0
                    : found->second.m_pendingWriteBytes;
            }

            void EpollDriver::StopConnection(Connection* connection) {
                if (m_impl == nullptr || connection == nullptr) return;
                const auto found = m_impl->m_connectionStates.find(connection);
                if (found == m_impl->m_connectionStates.end()) return;

                found->second.m_closing = true;
                found->second.m_readEnabled = false;
                const std::uint64_t registrationId = found->second.m_registrationId; // erase 前固定内核身份
                RemoveOperationRegistration(registrationId);
                if (found->second.m_datagram) {
                    std::shared_ptr<Connection> snapshot = found->second.m_connection;
                    std::deque<EpollDatagramWriteState> abandoned;
                    abandoned.swap(found->second.m_datagramWrites);
                    const std::size_t abandonedCount = abandoned.size();
                    found->second.m_pendingWriteBytes = 0;
                    m_impl->m_pendingDatagramSends = abandonedCount >= m_impl->m_pendingDatagramSends
                        ? 0
                        : m_impl->m_pendingDatagramSends - abandonedCount;
                    m_impl->m_connectionStates.erase(found);
                    for (const EpollDatagramWriteState& datagram : abandoned) {
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

            bool EpollDriver::HasPendingShutdownCompletions() const noexcept {
                return false;
            }

            std::uint64_t EpollDriver::CompletedOperationCount() const noexcept {
                return m_impl ? m_impl->m_completedOperations : 0;
            }

            std::size_t EpollDriver::ProvidedBufferCount() const noexcept {
                return 0;
            }

            std::uint64_t EpollDriver::ReadSubmissionCount() const noexcept {
                return m_impl ? m_impl->m_readSubmissions : 0;
            }

            std::uint64_t EpollDriver::AcceptSubmissionCount() const noexcept {
                return m_impl ? m_impl->m_acceptSubmissions : 0;
            }

            bool EpollDriver::ReceiveBundleEnabled() const noexcept {
                return false;
            }

            CompletionStats EpollDriver::GetCompletionStats() const noexcept {
                CompletionStats stats{}; // io_uring 专属能力保持默认零值
                if (m_impl == nullptr) return stats;
                stats.completedOperations = m_impl->m_completedOperations;
                stats.connectSubmissions = m_impl->m_connectSubmissions;
                stats.connectCancelSubmissions = m_impl->m_connectCancelSubmissions;
                stats.pendingConnectOperations = m_impl->m_connectStates.size();
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

            const char* EpollDriver::BackendName() const noexcept {
                return "epoll-level-completion";
            }
        }
    }
}
