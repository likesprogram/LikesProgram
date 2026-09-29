#include "net/platform/linux/IoUringPoller.hpp"
#include "net/BufferLeaseAccess.hpp"
#include "net/DatagramBatchPolicy.hpp"
#include "net/DatagramCompletionPolicy.hpp"
#include "net/OperationCancellationPolicy.hpp"
#include "net/PollerAccess.hpp"
#include "net/platform/SocketOps.hpp"
#include "net/platform/linux/ProvidedBufferPolicy.hpp"
#include "net/platform/linux/ProvidedBufferPool.hpp"
#include <LikesProgram/Net/Address.hpp>
#include <LikesProgram/Net/BufferChain.hpp>
#include <LikesProgram/Net/Connection.hpp>

#if defined(__linux__)

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <poll.h>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <liburing.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
    constexpr std::size_t kProvidedBufferSize = 8 * 1024; // 单个 TCP 读完成的最大连续批次
    constexpr std::size_t kMaximumProvidedBufferMemory = 64 * 1024 * 1024; // TCP/UDP group 总峰值
    constexpr std::uint16_t kFirstProvidedBufferGroup = 1; // 0 保留为未分配 generation

    unsigned int NextPowerOfTwo(unsigned int value) noexcept {
        unsigned int result = 1; // liburing 队列与 buffer ring 都要求二次幂容量
        while (result < value && result < 4096) result <<= 1;
        return result;
    }

    unsigned int DetectRingEntries() noexcept {
        const unsigned int cpuCount = std::max(1u, std::thread::hardware_concurrency()); // 运行时 CPU 规格
        const unsigned int scaled = cpuCount > 32 ? 4096u : cpuCount * 128u; // 高规格机器限制单 loop 内存
        return std::clamp(NextPowerOfTwo(scaled), 512u, 4096u);
    }

    unsigned int DetectProvidedBufferCount(unsigned int ringEntries) noexcept {
        // buffer 数随 SQ 容量变化，并用上下界避免低规格饥饿或高规格无界占用。
        return std::clamp(NextPowerOfTwo(ringEntries / 8), 64u, 512u);
    }

    short ToPollMask(LikesProgram::Net::IOEvent events) noexcept {
        short mask = POLLERR | POLLHUP; // 错误与挂断始终需要转换成 completion
        if (LikesProgram::Net::HasEvent(events, LikesProgram::Net::IOEvent::Read)) mask |= POLLIN | POLLPRI;
        if (LikesProgram::Net::HasEvent(events, LikesProgram::Net::IOEvent::Write)) mask |= POLLOUT;
#ifdef POLLRDHUP
        if (LikesProgram::Net::HasEvent(events, LikesProgram::Net::IOEvent::Read)) mask |= POLLRDHUP;
#endif
        return mask;
    }

    LikesProgram::Net::IOEvent FromPollMask(int mask) noexcept {
        LikesProgram::Net::IOEvent events = LikesProgram::Net::IOEvent::None; // 通用 completion 事件集合
        if ((mask & (POLLIN | POLLPRI)) != 0) events |= LikesProgram::Net::IOEvent::Read;
        if ((mask & POLLOUT) != 0) events |= LikesProgram::Net::IOEvent::Write;
#ifdef POLLRDHUP
        if ((mask & POLLRDHUP) != 0) events |= LikesProgram::Net::IOEvent::Close;
#endif
        if ((mask & POLLHUP) != 0) events |= LikesProgram::Net::IOEvent::Close;
        if ((mask & (POLLERR | POLLNVAL)) != 0) events |= LikesProgram::Net::IOEvent::Error;
        return events;
    }
}

namespace LikesProgram {
    namespace Net {
        namespace Internal {
            struct IoUringPoller::IoUringPollerImpl {
                enum class OperationType {
                    Channel,
                    Accept,
                    Connect,
                    Read,
                    Write,
                    DatagramRead,
                    DatagramWrite,
                    Timeout
                };

                struct Operation {
                    OperationType m_type = OperationType::Channel; // CQE 分派类型
                    void* m_state = nullptr;                       // 对应状态地址在 map value 生命周期内稳定
                };

                struct ChannelState {
                    explicit ChannelState(Channel* value)
                        : m_channel(value),
                        m_operation{ OperationType::Channel, this } {
                    }

                    Channel* m_channel = nullptr;                 // Channel 生命周期由 EventLoop/Server 管理
                    IOEvent m_events = IOEvent::None;             // 下一次提交使用的关注集合
                    bool m_outstanding = false;                   // 是否存在未完成 poll SQE
                    bool m_cancelRequested = false;               // 防止重复提交取消操作
                    bool m_cancelPending = false;                 // Update/Remove 首次 SQ 满后继续重试
                    bool m_queued = false;                        // 是否已进入待提交队列
                    bool m_closing = false;                       // 删除后只等待旧 CQE 回收
                    Operation m_operation;                        // poll SQE/CQE 稳定标签
                };

                struct ProvidedBufferGeneration {
                    std::uint16_t m_groupId = 0;                  // SQE 与 CQE 共同绑定的 buffer group
                    std::uint32_t m_bufferCount = 0;              // 当前 generation 的二次幂 ring 容量
                    std::size_t m_bufferSize = 0;                 // 当前 generation 的单 buffer 容量
                    io_uring_buf_ring* m_bufferRing = nullptr;    // 内核注册的 provided-buffer ring
                    ProvidedBufferPool* m_bufferPool = nullptr;   // lease 可延长的稳定字节存储
                    std::uint16_t m_cachedBufferHead = 0;         // bundle 模式下一条待读 ring entry
                    std::size_t m_outstandingReads = 0;           // 尚未收到 terminal CQE 的 read 数
                    bool m_receiveBundleEnabled = false;          // 当前 generation 是否可解析 bundle
                    bool m_retiring = false;                      // 不再接收新 read，只等待旧 CQE
                };

                struct DatagramBufferGeneration {
                    std::uint16_t m_groupId = 0;                  // UDP multishot 独占的稳定 buffer group
                    std::uint32_t m_bufferCount = 0;              // 固定二次幂 ring entry 数
                    std::size_t m_bufferSize = 0;                 // recvmsg 元数据与完整数据报总容量
                    io_uring_buf_ring* m_bufferRing = nullptr;    // 内核注册的 UDP provided-buffer ring
                    ProvidedBufferPool* m_bufferPool = nullptr;   // lease 可跨 Poller 生命周期持有的存储
                    bool m_registered = false;                    // true 表示 ring 仍由当前 io_uring 注册
                };

                struct ConnectionState;

                struct DatagramWriteState {
                    DatagramWriteState(
                        ConnectionState* owner,
                        DatagramWriteTarget target,
                        const Address& peer,
                        Buffer&& buffer)
                        : m_owner(owner),
                        m_peer(peer),
                        m_buffer(std::move(buffer)),
                        m_queueCost(DatagramQueueCost(m_buffer.ReadableBytes())),
                        m_target(target),
                        m_operation{ OperationType::DatagramWrite, this } {
                    }

                    ConnectionState* m_owner = nullptr;           // CQE 反查所属连接状态
                    Address m_peer;                               // 显式 peer 的 sockaddr；connected send 时为空
                    Buffer m_buffer;                              // 单个完整数据报 payload 所有权
                    std::size_t m_queueCost = 0;                  // 空包也占一单位的背压成本
                    DatagramWriteTarget m_target = DatagramWriteTarget::Invalid; // connected send 或显式 sendmsg
                    iovec m_iovec{};                              // 指向当前 Buffer 可读区的稳定描述符
                    msghdr m_message{};                           // 指向 peer/iovec 的 sendmsg 参数
                    bool m_submitted = false;                     // SQE 已发布后节点不得提前释放
                    bool m_completed = false;                     // CQE 可乱序到达，FIFO 头连续回收
                    bool m_counted = true;                        // pending 统计只允许扣减一次
                    int m_completionResult = 0;                   // 当前数据报的原始 CQE res
                    std::uint64_t m_batchSequence = 0;            // 诊断同一 linked SQE 批次
                    Operation m_operation;                        // 当前数据报独占的稳定 send CQE 标签
                };

                struct ConnectionState {
                    explicit ConnectionState(std::shared_ptr<Connection> value)
                        : m_connection(std::move(value)),
                        m_fd(m_connection ? m_connection->GetSocket() : kInvalidSocket),
                        m_datagram(m_connection
                            && m_connection->GetTransportKind() == TransportKind::Udp),
                        m_maxDatagramBytes(m_connection
                            ? PollerAccess::MaxDatagramBytes(*m_connection)
                            : 0),
                        m_datagramReadBuffer(0),
                        m_readOperation{
                            m_datagram ? OperationType::DatagramRead : OperationType::Read,
                            this },
                        m_writeOperation{ OperationType::Write, this } {
                    }

                    std::shared_ptr<Connection> m_connection;     // CQE 回收前延长连接生命周期
                    SocketType m_fd = kInvalidSocket;             // 提交操作使用的稳定 fd 快照
                    bool m_datagram = false;                      // UDP 使用 one-shot recvmsg/sendmsg 状态
                    std::size_t m_maxDatagramBytes = 0;           // 单个 recvmsg 的业务配置容量
                    Buffer m_datagramReadBuffer;                  // issuer 回调期间借给 Connection 的接收存储
                    sockaddr_storage m_datagramPeer{};            // recvmsg 返回的发送方地址
                    iovec m_datagramReadIovec{};                  // 指向接收 Buffer 当前可写区
                    msghdr m_datagramReadMessage{};               // recvmsg 稳定参数与输出 flags
                    msghdr m_datagramMultishotMessage{};          // multishot recvmsg 的固定输出布局模板
                    std::list<DatagramWriteState> m_datagramWrites; // 地址稳定的 sendmsg FIFO 节点
                    bool m_datagramMultishot = false;             // 当前 read operation 使用 UDP 快路径
                    bool m_datagramReadWaitingForBuffer = false;  // ENOBUFS 后等待 issuer 回填至少一个 token
                    bool m_closing = false;                       // 关闭后只回收 completion
                    bool m_readEnabled = true;                    // 业务背压读开关
                    bool m_readOutstanding = false;               // multishot recv 是否仍存活
                    bool m_readCancelRequested = false;           // 防止重复取消同一 multishot
                    ProvidedBufferGeneration* m_readGeneration = nullptr; // 当前 read 固定绑定的 generation
                    bool m_writeOutstanding = false;              // 当前是否存在未完成 send
                    bool m_writeCancelRequested = false;          // 防止重复取消同一 send
                    std::size_t m_datagramWriteOutstanding = 0;   // 当前 linked 批次未回收 CQE 数
                    bool m_datagramWriteErrorReported = false;    // 同一失败链只通知首个真实错误
                    bool m_dispatching = false;                   // CQE 回调栈内禁止销毁状态
                    std::size_t m_pendingWriteBytes = 0;           // 全部异步写块剩余字节数
                    BufferChain m_writeChain;                      // CQE 完成前持有全部稳定发送 Buffer 段
                    Operation m_readOperation;                     // multishot recv 标签
                    Operation m_writeOperation;                    // send 标签
                };

                struct AcceptState {
                    AcceptState(SocketType fd, AcceptCallback callback)
                        : m_fd(fd),
                        m_callback(std::move(callback)),
                        m_operation{ OperationType::Accept, this } {
                    }

                    SocketType m_fd = kInvalidSocket;              // listener fd 在取消 completion 回收前保持稳定
                    AcceptCallback m_callback;                     // 每个成功 accept completion 的消费入口
                    bool m_outstanding = false;                    // multishot accept 是否仍由内核持有
                    bool m_cancelRequested = false;                // 防止重复提交 cancel
                    bool m_closing = false;                        // 停止后只回收旧 completion
                    bool m_dispatching = false;                    // accept 回调栈内禁止销毁状态
                    Operation m_operation;                         // accept SQE/CQE 稳定标签
                };

                struct ConnectState {
                    ConnectState(
                        ConnectId connectId,
                        SocketType fd,
                        const Address& remoteAddress,
                        ConnectCallback callback)
                        : m_connectId(connectId),
                        m_fd(fd),
                        m_remoteAddress(remoteAddress),
                        m_callback(std::move(callback)),
                        m_operation{ OperationType::Connect, this } {
                    }

                    ConnectId m_connectId = InvalidConnectId;      // 调用方取消连接使用的稳定标识
                    SocketType m_fd = kInvalidSocket;              // 成功回调前由 Poller 独占 socket
                    Address m_remoteAddress;                       // connect SQE 完成前保持 sockaddr 存储稳定
                    ConnectCallback m_callback;                    // CQE 在 issuer 线程分派连接结果
                    bool m_outstanding = false;                    // connect SQE 是否尚未回收 CQE
                    bool m_cancelRequested = false;                // 防止重复提交 connect cancel
                    bool m_closing = false;                        // 取消后抑制用户回调并关闭 socket
                    bool m_dispatching = false;                    // callback 栈内禁止删除状态
                    Operation m_operation;                         // connect SQE/CQE 稳定标签
                };

                struct TimeoutState {
                    // 创建一次定时 operation 的稳定 CQE 状态。
                    TimeoutState(TimeoutId timeoutId, TimeoutCallback callback)
                        : m_timeoutId(timeoutId),
                        m_callback(std::move(callback)),
                        m_operation{ OperationType::Timeout, this } {
                    }

                    TimeoutId m_timeoutId = InvalidTimeoutId;      // 调用方取消 timer 使用的稳定标识
                    TimeoutCallback m_callback;                    // 到期后在 EventLoop issuer 线程执行
                    __kernel_timespec m_duration{};                // SQE 提交前保持相对超时参数有效
                    bool m_outstanding = false;                    // timeout SQE 是否尚未回收 CQE
                    bool m_cancelRequested = false;                // 防止重复提交 timeout cancel
                    bool m_closing = false;                        // 取消后只回收原 timeout CQE
                    bool m_dispatching = false;                    // callback 栈内禁止删除状态
                    Operation m_operation;                         // timeout SQE/CQE 稳定标签
                };

                io_uring_sqe* AcquireSqe() {
                    io_uring_sqe* sqe = io_uring_get_sqe(&m_ring); // 优先积累到当前 SQ 批次
                    if (sqe != nullptr) return sqe;

                    if (io_uring_submit(&m_ring) < 0) return nullptr;
                    return io_uring_get_sqe(&m_ring);
                }

                void QueueChannel(ChannelState& state) {
                    if (state.m_queued || state.m_outstanding || state.m_closing
                        || state.m_channel == nullptr || state.m_events == IOEvent::None) return;

                    state.m_queued = true;
                    m_pendingChannels.push_back(&state);
                }

                bool SubmitChannel(ChannelState& state) {
                    if (state.m_closing || state.m_channel == nullptr || state.m_events == IOEvent::None) return false;

                    io_uring_sqe* sqe = AcquireSqe(); // listener、wakeup 与兼容 transport 统一产出 CQE
                    if (sqe == nullptr) return false;
                    io_uring_prep_poll_add(
                        sqe,
                        static_cast<int>(state.m_channel->GetSocket()),
                        ToPollMask(state.m_events));
                    io_uring_sqe_set_data(sqe, &state.m_operation);
                    state.m_outstanding = true;
                    state.m_cancelRequested = false;
                    state.m_cancelPending = false;
                    return true;
                }

                void DrainChannelSubmissions() {
                    std::vector<ChannelState*> pending; // 允许 SubmitChannel 再次排队而不修改当前遍历容器
                    pending.swap(m_pendingChannels);
                    for (ChannelState* state : pending) {
                        if (state == nullptr) continue;
                        state->m_queued = false;
                        if (state->m_closing) {
                            if (!state->m_outstanding) m_channelStates.erase(state->m_channel);
                            continue;
                        }
                        if (!SubmitChannel(*state)) QueueChannel(*state);
                    }
                }

                bool SubmitRead(ConnectionState& state) {
                    if (state.m_closing || !state.m_readEnabled || state.m_readOutstanding
                        || !state.m_connection || state.m_readGeneration != nullptr) return false;

                    if (state.m_datagram) {
                        if (state.m_maxDatagramBytes == 0) return false;

                        DatagramBufferGeneration* generation =
                            m_datagramBufferGeneration.get(); // UDP 快路径只绑定固定独立 group
                        if (generation != nullptr
                            && generation->m_registered
                            && generation->m_bufferRing != nullptr
                            && generation->m_bufferPool != nullptr
                            && !m_datagramMultishotDisabled) {
                            const std::size_t availableBuffers =
                                generation->m_bufferPool->CurrentAvailableCount(); // ENOBUFS 后的权威水位
                            if (!ShouldRetryDatagramRead(
                                    state.m_datagramReadWaitingForBuffer,
                                    availableBuffers)) return false;

                            io_uring_sqe* sqe = AcquireSqe(); // 每连接只保留一个长期 recvmsg SQE
                            if (sqe == nullptr) return false;

                            state.m_datagramMultishotMessage = {};
                            state.m_datagramMultishotMessage.msg_namelen =
                                sizeof(sockaddr_storage); // 固定 name 区容纳 IPv4/IPv6 peer
                            io_uring_prep_recvmsg_multishot(
                                sqe,
                                state.m_fd,
                                &state.m_datagramMultishotMessage,
                                0);
                            sqe->buf_group = generation->m_groupId;
                            sqe->flags |= IOSQE_BUFFER_SELECT;
                            io_uring_sqe_set_data(sqe, &state.m_readOperation);
                            state.m_readOutstanding = true;
                            state.m_readCancelRequested = false;
                            state.m_datagramMultishot = true;
                            state.m_datagramReadWaitingForBuffer = false;
                            m_datagramMultishotEnabled.store(true, std::memory_order_relaxed);
                            m_readSubmissions.fetch_add(1, std::memory_order_relaxed);
                            return true;
                        }

                        state.m_datagramReadWaitingForBuffer = false; // fallback 不依赖 provided-buffer 水位
                        std::uint8_t* storage = nullptr; // recvmsg SQE 指向的固定 Buffer 可写区
                        try {
                            state.m_datagramReadBuffer.RetrieveAll();
                            storage = state.m_datagramReadBuffer.PrepareWrite(
                                state.m_maxDatagramBytes);
                        }
                        catch (...) {
                            return false;
                        }

                        std::memset(&state.m_datagramPeer, 0, sizeof(state.m_datagramPeer));
                        state.m_datagramReadIovec = {};
                        state.m_datagramReadIovec.iov_base = storage;
                        state.m_datagramReadIovec.iov_len = state.m_maxDatagramBytes;
                        state.m_datagramReadMessage = {};
                        state.m_datagramReadMessage.msg_name = &state.m_datagramPeer;
                        state.m_datagramReadMessage.msg_namelen = sizeof(state.m_datagramPeer);
                        state.m_datagramReadMessage.msg_iov = &state.m_datagramReadIovec;
                        state.m_datagramReadMessage.msg_iovlen = 1;

                        io_uring_sqe* sqe = AcquireSqe(); // UDP 每次只提交一个边界明确的 recvmsg
                        if (sqe == nullptr) return false;
                        io_uring_prep_recvmsg(
                            sqe,
                            state.m_fd,
                            &state.m_datagramReadMessage,
                            MSG_TRUNC);
                        io_uring_sqe_set_data(sqe, &state.m_readOperation);
                        state.m_readOutstanding = true;
                        state.m_readCancelRequested = false;
                        state.m_datagramMultishot = false;
                        m_readSubmissions.fetch_add(1, std::memory_order_relaxed);
                        return true;
                    }

                    if (!m_currentBufferGeneration) return false;

                    ProvidedBufferGeneration* generation = m_currentBufferGeneration.get(); // 本次 SQE 的稳定 group
                    if (generation->m_retiring || generation->m_bufferRing == nullptr
                        || generation->m_bufferPool == nullptr) return false;

                    io_uring_sqe* sqe = AcquireSqe(); // 一个连接只保留一个长期 multishot recv
                    if (sqe == nullptr) return false;
                    io_uring_prep_recv_multishot(sqe, state.m_fd, nullptr, 0, 0);
                    sqe->buf_group = generation->m_groupId;
                    sqe->flags |= IOSQE_BUFFER_SELECT;
#ifdef IORING_RECVSEND_BUNDLE
                    if (generation->m_receiveBundleEnabled) sqe->ioprio |= IORING_RECVSEND_BUNDLE;
#endif
                    io_uring_sqe_set_data(sqe, &state.m_readOperation);
                    state.m_readOutstanding = true;
                    state.m_readCancelRequested = false;
                    state.m_readGeneration = generation;
                    ++generation->m_outstandingReads;
                    m_readSubmissions.fetch_add(1, std::memory_order_relaxed);
                    return true;
                }

                void CompleteReceivedBuffers(
                    ConnectionState& state,
                    ProvidedBufferGeneration& generation,
                    const io_uring_cqe& completion) {
                    std::size_t remaining = static_cast<std::size_t>(completion.res); // bundle 全部 payload 字节数
                    const unsigned int firstBufferId = completion.flags >> IORING_CQE_BUFFER_SHIFT;
                    const unsigned int bufferMask = io_uring_buf_ring_mask(
                        generation.m_bufferCount); // 当前 generation 的环形掩码
                    std::uint64_t bundleBufferCount = 0; // 当前 CQE 实际跨越的 provided-buffer 数

                    do {
                        unsigned int bufferId = firstBufferId; // 单 buffer completion 直接使用 CQE 返回 id
                        std::size_t segmentBytes = remaining; // 当前 BufferLease 可见字节数
                        if (generation.m_receiveBundleEnabled) {
                            const unsigned int ringIndex = generation.m_cachedBufferHead & bufferMask; // 当前旧/新 ring entry
                            const io_uring_buf& entry = generation.m_bufferRing->bufs[ringIndex];
                            bufferId = entry.bid;
                            segmentBytes = std::min<std::size_t>(remaining, entry.len);
                        }

                        ++generation.m_cachedBufferHead;
                        ++bundleBufferCount;
                        remaining -= segmentBytes;
                        if (state.m_closing || segmentBytes == 0) {
                            RecycleBuffer(generation, bufferId);
                            continue;
                        }

                        generation.m_bufferPool->RetainLease(bufferId); // lease 固定延长原 generation pool
                        BufferLease lease = BufferLeaseAccess::Adopt(
                            BufferAddress(generation, bufferId),
                            segmentBytes,
                            generation.m_bufferPool,
                            bufferId,
                            &IoUringPollerImpl::ReleaseProvidedBuffer);
                        PollerAccess::CompleteRead(*state.m_connection, std::move(lease));
                    } while (generation.m_receiveBundleEnabled && remaining > 0);

                    if (!generation.m_receiveBundleEnabled) return;

                    // feature=true 的成功 recv CQE 均按 bundle head 语义解释，即使本次只有一个 buffer。
                    m_receiveBundleCompletions.fetch_add(1, std::memory_order_relaxed);
                    m_receiveBundleBuffers.fetch_add(bundleBufferCount, std::memory_order_relaxed);
                    std::uint64_t maximumBuffers = m_maximumReceiveBundleBuffers.load(
                        std::memory_order_relaxed); // 多连接 completion 共享的历史峰值
                    while (maximumBuffers < bundleBufferCount
                        && !m_maximumReceiveBundleBuffers.compare_exchange_weak(
                            maximumBuffers,
                            bundleBufferCount,
                            std::memory_order_relaxed,
                            std::memory_order_relaxed)) {
                    }
                }

                // 解析一个 UDP multishot CQE，并把 payload lease 直接交付业务回调。
                bool CompleteDatagramReceivedBuffer(
                    ConnectionState& state,
                    DatagramBufferGeneration& generation,
                    const io_uring_cqe& completion) {
                    const bool hasBuffer = (completion.flags & IORING_CQE_F_BUFFER) != 0;
                    const unsigned int bufferId = completion.flags >> IORING_CQE_BUFFER_SHIFT; // 内核选择 token
                    if (!hasBuffer || bufferId >= generation.m_bufferCount
                        || completion.res < 0
                        || static_cast<std::size_t>(completion.res) > generation.m_bufferSize
                        || generation.m_bufferPool == nullptr) return false;

                    std::uint8_t* storage = generation.m_bufferPool->BufferAddress(bufferId); // 元数据起点
                    if (storage == nullptr) return false;
                    io_uring_recvmsg_out* output = io_uring_recvmsg_validate(
                        storage,
                        completion.res,
                        &state.m_datagramMultishotMessage);
                    if (output == nullptr || output->namelen == 0
                        || output->namelen > sizeof(sockaddr_storage)
                        || output->controllen != 0) return false;

                    sockaddr_storage peerStorage{}; // 只复制内核声明的已验证 name 字节
                    std::memcpy(
                        &peerStorage,
                        io_uring_recvmsg_name(output),
                        output->namelen);
                    Address peer(
                        peerStorage,
                        static_cast<SocketLength>(output->namelen));
                    if (!peer.IsValid()) return false;

                    const std::size_t availableBytes = io_uring_recvmsg_payload_length(
                        output,
                        completion.res,
                        &state.m_datagramMultishotMessage); // 当前 provided buffer 内实际 payload
                    const auto received = InterpretDatagramBatchReceive(
                        output->payloadlen,
                        availableBytes,
                        state.m_maxDatagramBytes,
                        (output->flags & MSG_TRUNC) != 0);
                    auto* payload = static_cast<std::uint8_t*>(
                        io_uring_recvmsg_payload(output, &state.m_datagramMultishotMessage));
                    if (payload == nullptr) return false;

                    generation.m_bufferPool->RetainLease(bufferId); // 回调后允许业务显式物化或延长 lease
                    BufferLease lease = BufferLeaseAccess::Adopt(
                        payload,
                        received.payloadBytes,
                        generation.m_bufferPool,
                        bufferId,
                        &IoUringPollerImpl::ReleaseProvidedBuffer);
                    Buffer input; // 单个 CQE 只构造一个数据报边界
                    input.Append(std::move(lease));

                    m_datagramReceiveCompletions.fetch_add(1, std::memory_order_relaxed);
                    m_datagramMultishotReceiveCompletions.fetch_add(1, std::memory_order_relaxed);
                    m_receivedDatagrams.fetch_add(1, std::memory_order_relaxed);
                    if (received.originalBytes == 0) {
                        m_zeroLengthDatagrams.fetch_add(1, std::memory_order_relaxed);
                    }
                    if (received.truncated) {
                        m_truncatedDatagrams.fetch_add(1, std::memory_order_relaxed);
                    }
                    PollerAccess::CompleteDatagram(
                        *state.m_connection,
                        input,
                        peer,
                        received.originalBytes,
                        received.truncated);
                    return true;
                }

                bool SubmitAccept(AcceptState& state) {
                    if (state.m_closing || state.m_outstanding || state.m_fd == kInvalidSocket) return false;

                    io_uring_sqe* sqe = AcquireSqe(); // listener 长期保持一个 multishot accept SQE
                    if (sqe == nullptr) return false;
                    io_uring_prep_multishot_accept(
                        sqe,
                        static_cast<int>(state.m_fd),
                        nullptr,
                        nullptr,
                        SOCK_NONBLOCK | SOCK_CLOEXEC);
                    io_uring_sqe_set_data(sqe, &state.m_operation);
                    state.m_outstanding = true;
                    state.m_cancelRequested = false;
                    m_acceptSubmissions.fetch_add(1, std::memory_order_relaxed);
                    return true;
                }

                bool SubmitWrite(ConnectionState& state) {
                    if (state.m_closing || state.m_writeOutstanding) return false;

                    if (state.m_datagram) {
                        if (state.m_datagramWrites.empty()) return false;

                        std::array<io_uring_sqe*, 16> submissions{}; // 先完整取得本批 SQE
                        std::array<DatagramWriteState*, 16> datagrams{}; // 与 SQE 一一对应的稳定节点
                        const std::size_t batchSize = DatagramSendBatchSize(
                            state.m_datagramWrites.size(),
                            io_uring_sq_space_left(&m_ring),
                            state.m_datagramWriteOutstanding != 0);
                        if (batchSize == 0) return false;

                        auto datagram = state.m_datagramWrites.begin(); // 当前队列只从未提交头开始新批次
                        for (std::size_t index = 0; index < batchSize; ++index, ++datagram) {
                            submissions[index] = io_uring_get_sqe(&m_ring);
                            datagrams[index] = &*datagram;
                            if (submissions[index] == nullptr || datagrams[index]->m_submitted) {
                                // 已取得项降级为无标签 NOP，绝不发布半条 linked send 链。
                                for (std::size_t prepared = 0; prepared <= index; ++prepared) {
                                    if (submissions[prepared] != nullptr) {
                                        io_uring_prep_nop(submissions[prepared]);
                                        io_uring_sqe_set_data(submissions[prepared], nullptr);
                                    }
                                }
                                return false;
                            }
                        }

                        const std::uint64_t batchSequence = m_nextDatagramBatchSequence++; // issuer 唯一批次号
                        for (std::size_t index = 0; index < batchSize; ++index) {
                            DatagramWriteState& item = *datagrams[index]; // SQE 引用节点内稳定描述符
                            io_uring_sqe* sqe = submissions[index];
                            if (item.m_target == DatagramWriteTarget::Connected) {
                                io_uring_prep_send(
                                    sqe,
                                    state.m_fd,
                                    item.m_buffer.Peek(),
                                    item.m_buffer.ReadableBytes(),
                                    MSG_NOSIGNAL | MSG_WAITALL);
                            }
                            else {
                                item.m_iovec = {};
                                item.m_iovec.iov_base =
                                    const_cast<std::uint8_t*>(item.m_buffer.Peek());
                                item.m_iovec.iov_len = item.m_buffer.ReadableBytes();
                                item.m_message = {};
                                item.m_message.msg_name =
                                    const_cast<sockaddr*>(item.m_peer.SockAddr());
                                item.m_message.msg_namelen = item.m_peer.Length();
                                item.m_message.msg_iov = &item.m_iovec;
                                item.m_message.msg_iovlen = 1;
                                io_uring_prep_sendmsg(
                                    sqe,
                                    state.m_fd,
                                    &item.m_message,
                                    MSG_NOSIGNAL | MSG_WAITALL);
                            }
                            if (index + 1 < batchSize) sqe->flags |= IOSQE_IO_LINK;
                            io_uring_sqe_set_data(sqe, &item.m_operation);
                            item.m_submitted = true;
                            item.m_batchSequence = batchSequence;
                        }

                        state.m_datagramWriteOutstanding = batchSize;
                        state.m_writeOutstanding = true;
                        state.m_writeCancelRequested = false;
                        state.m_datagramWriteErrorReported = false;
                        m_datagramSendBatchSubmissions.fetch_add(1, std::memory_order_relaxed);
                        std::size_t maximum = m_maximumDatagramSendBatch.load(
                            std::memory_order_relaxed); // 多连接共享历史峰值
                        while (maximum < batchSize
                            && !m_maximumDatagramSendBatch.compare_exchange_weak(
                                maximum,
                                batchSize,
                                std::memory_order_relaxed,
                                std::memory_order_relaxed)) {
                        }
                        return true;
                    }

                    if (state.m_writeChain.Empty()) return false;

                    const BufferSlice active = state.m_writeChain.Segment(0); // 当前 send 只引用链首连续段
                    if (active.Empty()) return false;

                    io_uring_sqe* sqe = AcquireSqe(); // 一个连接同时只保留一个 send SQE
                    if (sqe == nullptr) return false;
                    io_uring_prep_send(
                        sqe,
                        state.m_fd,
                        active.Data(),
                        active.Size(),
                        MSG_NOSIGNAL);
                    io_uring_sqe_set_data(sqe, &state.m_writeOperation);
                    state.m_writeOutstanding = true;
                    state.m_writeCancelRequested = false;
                    return true;
                }

                // 返回 linked 批次中第一个仍等待 CQE 的节点，用作唯一 cancel 锚点。
                DatagramWriteState* FirstIncompleteDatagramWrite(
                    ConnectionState& state) noexcept {
                    for (DatagramWriteState& datagram : state.m_datagramWrites) {
                        if (datagram.m_submitted && !datagram.m_completed) return &datagram;
                    }
                    return nullptr;
                }

                bool Cancel(Operation& operation) {
                    io_uring_sqe* sqe = AcquireSqe(); // cancel CQE 不带业务标签，原操作 CQE 负责回收状态
                    if (sqe == nullptr) return false;
                    io_uring_prep_cancel(sqe, &operation, 0);
                    io_uring_sqe_set_data(sqe, nullptr);
                    return true;
                }

                bool SubmitConnectCancel(ConnectState& state) noexcept {
                    if (!Cancel(state.m_operation)) return false;
                    m_connectCancelSubmissions.fetch_add(1, std::memory_order_relaxed);
                    return true;
                }

                // SQ 暂满时在后续 Flush 重试 connect cancel，避免超时请求长期占用 fd。
                void CancelPendingConnects() noexcept {
                    for (auto& item : m_connectStates) {
                        ConnectState& state = *item.second; // map value 在原 CQE 回收前保持稳定
                        if (state.m_closing && state.m_outstanding && !state.m_cancelRequested) {
                            state.m_cancelRequested = SubmitConnectCancel(state);
                        }
                    }
                }

                // 判断候选 group 是否仍被当前或等待 CQE 的 generation 注册。
                bool BufferGroupInUse(std::uint16_t groupId) const noexcept {
                    if (groupId == 0) return true;
                    if (m_datagramBufferGeneration
                        && m_datagramBufferGeneration->m_groupId == groupId) return true;
                    if (m_currentBufferGeneration
                        && m_currentBufferGeneration->m_groupId == groupId) return true;
                    for (const auto& generation : m_retiringBufferGenerations) {
                        if (generation && generation->m_groupId == groupId) return true;
                    }
                    return false;
                }

                // 从 16 位空间循环分配一个当前未注册的 buffer group。
                std::uint16_t AllocateBufferGroup() noexcept {
                    for (std::uint32_t attempt = 0;
                        attempt < (std::numeric_limits<std::uint16_t>::max)();
                        ++attempt) {
                        std::uint16_t candidate = m_nextBufferGroup++; // 0 在回绕时继续跳过
                        if (candidate == 0) candidate = m_nextBufferGroup++;
                        if (!BufferGroupInUse(candidate)) return candidate;
                    }
                    return 0;
                }

                // 校验新 generation 参数及包含 retained pool 的总内存峰值。
                bool TryGetExistingGenerationBytes(std::size_t& existingBytes) const noexcept {
                    existingBytes = 0; // current、retiring 与 retained 都计入峰值
                    const auto addGeneration = [&existingBytes](const ProvidedBufferGeneration* generation) {
                        if (generation == nullptr) return true;
                        if (generation->m_bufferSize
                            > (std::numeric_limits<std::size_t>::max)()
                                / generation->m_bufferCount) return false;
                        const std::size_t bytes = generation->m_bufferSize * generation->m_bufferCount;
                        if (existingBytes > (std::numeric_limits<std::size_t>::max)() - bytes) return false;
                        existingBytes += bytes;
                        return true;
                    };
                    if (!addGeneration(m_currentBufferGeneration.get())) return false;
                    for (const auto& generation : m_retiringBufferGenerations) {
                        if (!addGeneration(generation.get())) return false;
                    }
                    for (const auto& generation : m_retainedBufferGenerations) {
                        if (!addGeneration(generation.get())) return false;
                    }
                    if (m_datagramBufferGeneration) {
                        const std::size_t datagramSize = m_datagramBufferGeneration->m_bufferSize;
                        const std::uint32_t datagramCount = m_datagramBufferGeneration->m_bufferCount;
                        std::size_t datagramBytes = 0; // helper 统一约束 UDP pool 乘法溢出
                        if (!TryMultiplyDatagramBufferBytes(
                                datagramSize,
                                datagramCount,
                                datagramBytes)
                            || datagramBytes == 0) return false;
                        if (existingBytes > (std::numeric_limits<std::size_t>::max)()
                            - datagramBytes) return false;
                        existingBytes += datagramBytes;
                    }
                    return true;
                }

                // 校验新 generation 参数及包含 retained pool 的总内存峰值。
                bool ValidateBufferDimensions(
                    std::size_t bufferSize,
                    std::uint32_t bufferCount) const noexcept {
                    std::size_t existingBytes = 0; // 注册期间新旧 group 必须同时计入内存上界
                    if (!TryGetExistingGenerationBytes(existingBytes)) return false;
                    return ProvidedBufferPolicy::CanCreate(
                        bufferSize,
                        bufferCount,
                        existingBytes);
                }

                // UDP buffer 允许包含 recvmsg 元数据，但仍与全部 TCP generation 共用 64 MiB 上界。
                bool ValidateDatagramBufferDimensions(
                    std::size_t bufferSize,
                    std::uint32_t bufferCount) const noexcept {
                    std::size_t existingBytes = 0; // 创建 UDP group 前保留全部 TCP 代内存
                    std::size_t datagramBytes = 0; // 专用 helper 保证失败时不执行溢出乘法
                    if (!TryMultiplyDatagramBufferBytes(
                            bufferSize,
                            bufferCount,
                            datagramBytes)
                        || datagramBytes == 0
                        || !TryGetExistingGenerationBytes(existingBytes)) return false;
                    return existingBytes <= kMaximumProvidedBufferMemory
                        && datagramBytes <= kMaximumProvidedBufferMemory - existingBytes;
                }

                // 分配 pool、注册 ring 并一次性填入全部 buffer entry。
                std::unique_ptr<ProvidedBufferGeneration> CreateBufferGeneration(
                    std::size_t bufferSize,
                    std::uint32_t bufferCount,
                    int& error) {
                    error = 0;
                    const std::uint16_t groupId = AllocateBufferGroup(); // 注册前预留唯一 16 位 group
                    if (groupId == 0) {
                        error = ENOSPC;
                        return {};
                    }

                    auto generation = std::make_unique<ProvidedBufferGeneration>(); // ring 与 pool 同代持有
                    generation->m_groupId = groupId;
                    generation->m_bufferCount = bufferCount;
                    generation->m_bufferSize = bufferSize;
                    generation->m_receiveBundleEnabled = m_receiveBundleFeatureEnabled;
                    generation->m_bufferPool = ProvidedBufferPool::Create(bufferSize, bufferCount);

                    int bufferError = 0; // liburing 通过输出参数返回 buffer ring 注册错误
                    generation->m_bufferRing = io_uring_setup_buf_ring(
                        &m_ring,
                        bufferCount,
                        groupId,
                        0,
                        &bufferError);
                    if (generation->m_bufferRing == nullptr) {
                        error = bufferError < 0 ? -bufferError : ENOMEM;
                        generation->m_bufferPool->Close();
                        generation->m_bufferPool = nullptr;
                        return {};
                    }

                    const int mask = io_uring_buf_ring_mask(bufferCount); // 每个 generation 独立填满 ring
                    for (std::uint32_t bufferId = 0; bufferId < bufferCount; ++bufferId) {
                        io_uring_buf_ring_add(
                            generation->m_bufferRing,
                            generation->m_bufferPool->BufferAddress(bufferId),
                            static_cast<unsigned int>(bufferSize),
                            static_cast<unsigned short>(bufferId),
                            mask,
                            bufferId);
                    }
                    io_uring_buf_ring_advance(generation->m_bufferRing, bufferCount);
                    if (generation->m_receiveBundleEnabled
                        && io_uring_buf_ring_head(
                            &m_ring,
                            groupId,
                            &generation->m_cachedBufferHead) != 0) {
                        // 单个 generation 无法读取 head 时只关闭它的 bundle，不污染其他 group。
                        generation->m_receiveBundleEnabled = false;
                        generation->m_cachedBufferHead = 0;
                    }
                    return generation;
                }

                // 注册固定 UDP group；失败只关闭快路径资源，不阻止 one-shot UDP 与 TCP 激活。
                std::unique_ptr<DatagramBufferGeneration> CreateDatagramBufferGeneration(
                    std::size_t bufferSize,
                    std::uint32_t bufferCount,
                    int& error) {
                    error = 0;
                    if (!ValidateDatagramBufferDimensions(bufferSize, bufferCount)) {
                        error = ENOMEM;
                        return {};
                    }

                    const std::uint16_t groupId = AllocateBufferGroup(); // 与全部 TCP generation 避免冲突
                    if (groupId == 0) {
                        error = ENOSPC;
                        return {};
                    }

                    auto generation = std::make_unique<DatagramBufferGeneration>(); // ring 与 pool 同生共退
                    generation->m_groupId = groupId;
                    generation->m_bufferCount = bufferCount;
                    generation->m_bufferSize = bufferSize;
                    generation->m_bufferPool = ProvidedBufferPool::Create(bufferSize, bufferCount);

                    int bufferError = 0; // liburing 以输出参数返回 ring 注册错误
                    generation->m_bufferRing = io_uring_setup_buf_ring(
                        &m_ring,
                        bufferCount,
                        groupId,
                        0,
                        &bufferError);
                    if (generation->m_bufferRing == nullptr) {
                        error = bufferError < 0 ? -bufferError : ENOMEM;
                        generation->m_bufferPool->Close();
                        generation->m_bufferPool = nullptr;
                        return {};
                    }

                    const int mask = io_uring_buf_ring_mask(bufferCount); // 初始水位一次性填满
                    for (std::uint32_t bufferId = 0; bufferId < bufferCount; ++bufferId) {
                        io_uring_buf_ring_add(
                            generation->m_bufferRing,
                            generation->m_bufferPool->BufferAddress(bufferId),
                            static_cast<unsigned int>(bufferSize),
                            static_cast<unsigned short>(bufferId),
                            mask,
                            bufferId);
                    }
                    io_uring_buf_ring_advance(generation->m_bufferRing, bufferCount);
                    generation->m_registered = true;
                    return generation;
                }

                // 注销不再有 read operation 的 buffer ring；失败时保留对象供后续重试。
                bool UnregisterBufferGeneration(ProvidedBufferGeneration& generation) noexcept {
                    if (generation.m_bufferRing == nullptr) return true;
                    const int result = io_uring_free_buf_ring(
                        &m_ring,
                        generation.m_bufferRing,
                        generation.m_bufferCount,
                        generation.m_groupId);
                    if (result < 0) return false;
                    generation.m_bufferRing = nullptr;
                    return true;
                }

                // 注销 UDP ring；活动 lease 仅保留 pool 存储，不再接触 io_uring。
                bool UnregisterDatagramBufferGeneration(
                    DatagramBufferGeneration& generation) noexcept {
                    if (!generation.m_registered || generation.m_bufferRing == nullptr) return true;
                    const int result = io_uring_free_buf_ring(
                        &m_ring,
                        generation.m_bufferRing,
                        generation.m_bufferCount,
                        generation.m_groupId);
                    if (result < 0) return false;
                    generation.m_bufferRing = nullptr;
                    generation.m_registered = false;
                    return true;
                }

                // 释放 generation 持有的 pool owner 引用，活动 lease 决定最终销毁时机。
                static void CloseBufferGenerationPool(ProvidedBufferGeneration& generation) noexcept {
                    if (generation.m_bufferPool == nullptr) return;
                    generation.m_bufferPool->Close(); // 晚到 lease 只延长旧存储，不再回填 ring
                    generation.m_bufferPool = nullptr;
                }

                // 关闭 UDP pool owner；最后一个活动 lease 负责释放底层连续存储。
                static void CloseDatagramBufferGenerationPool(
                    DatagramBufferGeneration& generation) noexcept {
                    if (generation.m_bufferPool == nullptr) return;
                    generation.m_bufferPool->Retire(); // 注销后禁止跨线程 token 回填旧 ring
                    generation.m_bufferPool->Close();
                    generation.m_bufferPool = nullptr;
                }

                // 返回指定 generation 内 token 对应的稳定字节地址。
                std::uint8_t* BufferAddress(
                    ProvidedBufferGeneration& generation,
                    unsigned int bufferId) noexcept {
                    return generation.m_bufferPool != nullptr
                        ? generation.m_bufferPool->BufferAddress(bufferId)
                        : nullptr;
                }

                // 把 token 回填到产生它的原 generation ring。
                void RecycleBuffer(
                    ProvidedBufferGeneration& generation,
                    unsigned int bufferId) noexcept {
                    std::uint8_t* address = BufferAddress(generation, bufferId); // 始终归还原 group
                    if (address == nullptr || generation.m_bufferRing == nullptr) return;

                    io_uring_buf_ring_add(
                        generation.m_bufferRing,
                        address,
                        static_cast<unsigned int>(generation.m_bufferSize),
                        static_cast<unsigned short>(bufferId),
                        io_uring_buf_ring_mask(generation.m_bufferCount),
                        0);
                    io_uring_buf_ring_advance(generation.m_bufferRing, 1);
                }

                // 未交付业务的 UDP token 在 issuer 线程立即回到原固定 group。
                void RecycleDatagramBuffer(
                    DatagramBufferGeneration& generation,
                    unsigned int bufferId) noexcept {
                    if (!generation.m_registered || generation.m_bufferPool == nullptr
                        || generation.m_bufferRing == nullptr
                        || bufferId >= generation.m_bufferCount) return;
                    std::uint8_t* address = generation.m_bufferPool->BufferAddress(bufferId);
                    if (address == nullptr) return;
                    io_uring_buf_ring_add(
                        generation.m_bufferRing,
                        address,
                        static_cast<unsigned int>(generation.m_bufferSize),
                        static_cast<unsigned short>(bufferId),
                        io_uring_buf_ring_mask(generation.m_bufferCount),
                        0);
                    io_uring_buf_ring_advance(generation.m_bufferRing, 1);
                }

                static void ReleaseProvidedBuffer(void* owner, std::uint32_t token) noexcept {
                    auto* pool = static_cast<ProvidedBufferPool*>(owner); // 任意线程只向原池排队 token
                    if (pool != nullptr) pool->ReleaseLease(token);
                }

                static void DrainProvidedBuffer(void* context, std::uint32_t token) noexcept {
                    auto* generation = static_cast<ProvidedBufferGeneration*>(context); // owner group 回填上下文
                    if (generation == nullptr || generation->m_bufferPool == nullptr
                        || generation->m_bufferRing == nullptr) return;
                    std::uint8_t* address = generation->m_bufferPool->BufferAddress(token);
                    if (address == nullptr) return;
                    io_uring_buf_ring_add(
                        generation->m_bufferRing,
                        address,
                        static_cast<unsigned int>(generation->m_bufferSize),
                        static_cast<unsigned short>(token),
                        io_uring_buf_ring_mask(generation->m_bufferCount),
                        0);
                    io_uring_buf_ring_advance(generation->m_bufferRing, 1);
                }

                static void DrainDatagramProvidedBuffer(void* context, std::uint32_t token) noexcept {
                    auto* generation = static_cast<DatagramBufferGeneration*>(context); // UDP 固定 group
                    if (generation == nullptr || generation->m_bufferPool == nullptr
                        || generation->m_bufferRing == nullptr || !generation->m_registered) return;
                    std::uint8_t* address = generation->m_bufferPool->BufferAddress(token);
                    if (address == nullptr) return;
                    io_uring_buf_ring_add(
                        generation->m_bufferRing,
                        address,
                        static_cast<unsigned int>(generation->m_bufferSize),
                        static_cast<unsigned short>(token),
                        io_uring_buf_ring_mask(generation->m_bufferCount),
                        0);
                    io_uring_buf_ring_advance(generation->m_bufferRing, 1);
                }

                void DrainReturnedBuffers() noexcept {
                    std::lock_guard<std::mutex> lock(m_bufferGenerationMutex); // 防止诊断线程看到退役中悬空对象
                    const auto drain = [](ProvidedBufferGeneration* generation) {
                        if (generation == nullptr || generation->m_bufferPool == nullptr
                            || generation->m_bufferRing == nullptr) return;
                        generation->m_bufferPool->DrainReturned(
                            generation,
                            &IoUringPollerImpl::DrainProvidedBuffer);
                    };
                    if (m_datagramBufferGeneration
                        && m_datagramBufferGeneration->m_bufferPool != nullptr
                        && m_datagramBufferGeneration->m_bufferRing != nullptr) {
                        m_datagramBufferGeneration->m_bufferPool->DrainReturned(
                            m_datagramBufferGeneration.get(),
                            &IoUringPollerImpl::DrainDatagramProvidedBuffer);
                    }
                    drain(m_currentBufferGeneration.get());
                    for (const auto& generation : m_retiringBufferGenerations) drain(generation.get());
                }

                // 重试首次因 SQ 暂满而未提交的 operation cancel。
                void RetryPendingOperationCancellations() noexcept {
                    ProvidedBufferGeneration* current = m_currentBufferGeneration.get(); // 新 read 只绑定当前代
                    for (auto& item : m_connectionStates) {
                        ConnectionState& state = *item.second;
                        const bool readWanted = !state.m_closing
                            && state.m_readEnabled
                            && (state.m_datagram
                                || state.m_readGeneration == current); // UDP 不绑定 provided-buffer generation
                        if (NeedsOperationCancellation(
                            state.m_readOutstanding,
                            state.m_readCancelRequested,
                            readWanted)) {
                            state.m_readCancelRequested = Cancel(state.m_readOperation);
                        }
                        if (NeedsOperationCancellation(
                            state.m_writeOutstanding,
                            state.m_writeCancelRequested,
                            !state.m_closing)) {
                            if (state.m_datagram) {
                                DatagramWriteState* datagram = FirstIncompleteDatagramWrite(state);
                                if (datagram != nullptr) {
                                    state.m_writeCancelRequested = Cancel(datagram->m_operation);
                                }
                            }
                            else {
                                state.m_writeCancelRequested = Cancel(
                                    state.m_writeOperation); // 必须引用内核收到的同一稳定标签
                            }
                        }
                    }

                    for (auto& item : m_acceptStates) {
                        AcceptState& state = *item.second; // listener close 前必须保证 cancel 最终入 SQ
                        if (NeedsOperationCancellation(
                            state.m_outstanding,
                            state.m_cancelRequested,
                            !state.m_closing)) {
                            state.m_cancelRequested = Cancel(state.m_operation);
                        }
                    }
                    for (auto& item : m_timeoutStates) {
                        TimeoutState& state = *item.second; // 已取消长 timer 不得一直占用 operation state
                        if (NeedsOperationCancellation(
                            state.m_outstanding,
                            state.m_cancelRequested,
                            !state.m_closing)) {
                            state.m_cancelRequested = Cancel(state.m_operation);
                        }
                    }
                    for (auto& item : m_channelStates) {
                        ChannelState& state = *item.second; // Update/Remove 在 SQ 恢复后重提 cancel
                        if (NeedsOperationCancellation(
                            state.m_outstanding,
                            state.m_cancelRequested,
                            !state.m_cancelPending)) {
                            state.m_cancelRequested = Cancel(state.m_operation);
                        }
                    }
                }

                // SQ 暂满后重试仍被业务需要的 UDP recvmsg/sendmsg。
                void RetryPendingDatagramOperations() noexcept {
                    for (auto& item : m_connectionStates) {
                        ConnectionState& state = *item.second; // issuer 线程独占 operation 状态
                        if (!state.m_datagram || state.m_closing) continue;
                        if (state.m_readEnabled && !state.m_readOutstanding) {
                            (void)SubmitRead(state);
                        }
                        if (!state.m_writeOutstanding && !state.m_datagramWrites.empty()) {
                            (void)SubmitWrite(state);
                        }
                    }
                }

                // terminal CQE 全部回收后注销旧 ring，并转入 retained pool 阶段。
                void RetireUnusedBufferGenerations() noexcept {
                    if (m_retiringBufferGenerations.empty()) return;
                    std::lock_guard<std::mutex> lock(m_bufferGenerationMutex); // 与跨线程统计快照串行销毁
                    auto generation = m_retiringBufferGenerations.begin();
                    while (generation != m_retiringBufferGenerations.end()) {
                        if (!*generation || (*generation)->m_outstandingReads != 0
                            || !UnregisterBufferGeneration(**generation)) {
                            ++generation;
                            continue;
                        }
                        (*generation)->m_bufferPool->Retire(); // 禁止晚到 lease 回填已注销 ring
                        m_retainedBufferGenerations.push_back(std::move(*generation)); // owner 继续计入内存上限
                        generation = m_retiringBufferGenerations.erase(generation);
                    }
                }

                // 最后一个旧 lease 释放后关闭 owner 引用并回收 retained 存储。
                void CleanupRetainedBufferGenerations() noexcept {
                    if (m_retainedBufferGenerations.empty()) return;
                    std::lock_guard<std::mutex> lock(m_bufferGenerationMutex); // lease 归零后释放 owner 与存储
                    auto generation = m_retainedBufferGenerations.begin();
                    while (generation != m_retainedBufferGenerations.end()) {
                        if (!*generation || (*generation)->m_bufferPool == nullptr
                            || (*generation)->m_bufferPool->ActiveLeaseCount() != 0) {
                            ++generation;
                            continue;
                        }
                        CloseBufferGenerationPool(**generation);
                        generation = m_retainedBufferGenerations.erase(generation);
                    }
                }

                unsigned int CurrentCompletionBudget() const noexcept {
                    const unsigned int baseBudget = m_completionBudget.load(std::memory_order_relaxed); // 反馈调节基线
                    if (baseBudget == 0 || m_ringEntries == 0) return 1;

                    const std::size_t scale = std::max<std::size_t>(
                        1,
                        m_ringEntries / baseBudget); // 由 SQ/provided-buffer 比例决定压力放大
                    const std::size_t pressureBudget = m_connectionStates.size() > m_ringEntries / scale
                        ? m_ringEntries
                        : m_connectionStates.size() * scale;
                    return static_cast<unsigned int>(std::clamp<std::size_t>(
                        pressureBudget,
                        baseBudget,
                        m_ringEntries));
                }

                void ObserveCompletionBatch(unsigned int processed, unsigned int requestedBudget) noexcept {
                    const std::uint64_t overflow = m_ring.cq.koverflow != nullptr
                        ? *m_ring.cq.koverflow
                        : 0; // 内核维护的 CQ overflow 累计值
                    const std::uint64_t previousOverflow = m_cqOverflowCount.exchange(
                        overflow,
                        std::memory_order_relaxed);
                    const unsigned int queued = io_uring_cq_ready(&m_ring); // 预算后仍排队的 CQE 数

                    if (processed > 0) {
                        m_completionBatchCount.fetch_add(1, std::memory_order_relaxed);
                        m_completionBatchItems.fetch_add(processed, std::memory_order_relaxed);
                        std::uint64_t peak = m_peakCompletionBatch.load(std::memory_order_relaxed);
                        while (peak < processed
                            && !m_peakCompletionBatch.compare_exchange_weak(
                                peak,
                                processed,
                                std::memory_order_relaxed,
                                std::memory_order_relaxed)) {
                        }
                    }

                    unsigned int baseBudget = m_completionBudget.load(std::memory_order_relaxed); // 当前调节基线
                    if (queued > 0 || overflow > previousOverflow || processed >= requestedBudget) {
                        const unsigned int growth = std::max(16U, baseBudget / 4U); // 压力下每轮最多增长 25%
                        m_completionBudget.store(
                            std::min(m_ringEntries, baseBudget + growth),
                            std::memory_order_relaxed);
                        m_lowBatchStreak = 0;
                        return;
                    }

                    if (processed > 0 && processed * 4U < requestedBudget) {
                        ++m_lowBatchStreak;
                        if (m_lowBatchStreak >= 64) {
                            const unsigned int shrink = std::max(8U, baseBudget / 8U); // 迟滞后缩减 12.5%
                            m_completionBudget.store(
                                std::max(64U, baseBudget > shrink ? baseBudget - shrink : 64U),
                                std::memory_order_relaxed);
                            m_lowBatchStreak = 0;
                        }
                    }
                    else {
                        m_lowBatchStreak = 0;
                    }
                }

                ProvidedBufferPolicy::Recommendation ObserveBufferPolicy() noexcept {
                    ProvidedBufferPolicy::Recommendation result{}; // changed=false 表示本轮不切换 generation
                    const std::uint64_t receiveCompletions = m_receiveCompletions.load(
                        std::memory_order_relaxed); // 成功 read CQE 累计值
                    const std::uint64_t enobufs = m_enobufsCompletions.load(
                        std::memory_order_relaxed); // buffer 饥饿累计值
                    const std::uint64_t overflow = m_cqOverflowCount.load(
                        std::memory_order_relaxed); // CQ overflow 累计值
                    const std::uint64_t receiveDelta = receiveCompletions - m_lastPolicyReceiveCompletions;
                    const std::uint64_t enobufsDelta = enobufs - m_lastPolicyEnobufsCompletions;
                    const std::uint64_t overflowDelta = overflow - m_lastPolicyCqOverflowCount;
                    if (receiveDelta < 64 && enobufsDelta == 0 && overflowDelta == 0) return result;

                    const std::uint64_t receivedBytes = m_receivedBytes.load(
                        std::memory_order_relaxed); // 与 completion delta 同一 issuer 窗口
                    const std::uint64_t receivedBytesDelta = receivedBytes - m_lastPolicyReceivedBytes;
                    m_lastPolicyReceiveCompletions = receiveCompletions;
                    m_lastPolicyReceivedBytes = receivedBytes;
                    m_lastPolicyEnobufsCompletions = enobufs;
                    m_lastPolicyCqOverflowCount = overflow;

                    std::lock_guard<std::mutex> lock(m_bufferGenerationMutex); // generation 指针与 pool 快照一致
                    const auto* generation = m_currentBufferGeneration.get(); // 只调节后续 read 的提交目标代
                    if (generation == nullptr || generation->m_bufferPool == nullptr) return result;
                    std::size_t existingBytes = 0; // 当前、退役和 retained 共同约束切换峰值
                    if (!TryGetExistingGenerationBytes(existingBytes)) return result;

                    ProvidedBufferPolicy::Observation observation{}; // 单窗口反馈不泄漏平台实现细节
                    observation.bufferSize = generation->m_bufferSize;
                    observation.bufferCount = generation->m_bufferCount;
                    observation.currentAvailable = generation->m_bufferPool->CurrentAvailableCount();
                    observation.activeLeases = generation->m_bufferPool->ActiveLeaseCount();
                    observation.enobufsDelta = enobufsDelta;
                    observation.cqOverflowDelta = overflowDelta;
                    observation.receivedBytes = receivedBytesDelta;
                    observation.receiveCompletions = receiveDelta;
                    observation.existingGenerationBytes = existingBytes;
                    m_bufferPolicyEvaluationCount.fetch_add(1, std::memory_order_relaxed);
                    result = m_bufferPolicy.Observe(observation);
                    if (result.changed) {
                        m_bufferPolicyRecommendationCount.fetch_add(1, std::memory_order_relaxed);
                    }
                    return result;
                }

                io_uring m_ring{};                                      // 当前 EventLoop 独占 SQ/CQ
                std::unique_ptr<DatagramBufferGeneration> m_datagramBufferGeneration; // UDP 固定 group
                std::unique_ptr<ProvidedBufferGeneration> m_currentBufferGeneration; // 新 read 提交目标
                std::vector<std::unique_ptr<ProvidedBufferGeneration>> m_retiringBufferGenerations; // 等待 terminal CQE
                std::vector<std::unique_ptr<ProvidedBufferGeneration>> m_retainedBufferGenerations; // 旧 lease 延长存储
                mutable std::mutex m_bufferGenerationMutex;             // 保护跨线程诊断与 generation 销毁
                std::unordered_map<Channel*, std::unique_ptr<ChannelState>> m_channelStates; // Channel 操作状态
                std::vector<ChannelState*> m_pendingChannels;           // 下一次 Flush 提交的 Channel 状态
                std::unordered_map<SocketType, std::unique_ptr<AcceptState>> m_acceptStates; // listener accept 状态
                std::unordered_map<ConnectId, std::unique_ptr<ConnectState>> m_connectStates; // outbound connect 状态
                std::unordered_map<Connection*, std::unique_ptr<ConnectionState>> m_connectionStates; // 直接 TCP 状态
                std::unordered_map<TimeoutId, std::unique_ptr<TimeoutState>> m_timeoutStates; // timer completion 状态
                ConnectId m_nextConnectId = 1;                       // 0 保留为无效 connect id
                TimeoutId m_nextTimeoutId = 1;                       // 0 保留为无效 timeout id
                std::uint64_t m_nextDatagramBatchSequence = 1;       // 0 保留为未提交节点
                std::atomic<std::uint64_t> m_completedOperations{ 0 };  // 已消费 CQE 总数
                std::atomic<std::uint64_t> m_connectSubmissions{ 0 };   // connect SQE 提交总数
                std::atomic<std::uint64_t> m_connectCancelSubmissions{ 0 }; // connect cancel SQE 提交总数
                std::atomic<std::size_t> m_pendingConnectOperations{ 0 }; // 尚未回收原 CQE 的 connect 数
                std::atomic<std::uint64_t> m_readSubmissions{ 0 };      // multishot recv 提交总数
                std::atomic<std::uint64_t> m_acceptSubmissions{ 0 };    // multishot accept 提交总数
                std::atomic<std::uint64_t> m_completionBatchCount{ 0 }; // 非空 CQE 批次数
                std::atomic<std::uint64_t> m_completionBatchItems{ 0 }; // 全部批次 CQE 数之和
                std::atomic<std::uint64_t> m_peakCompletionBatch{ 0 };  // 单次 Poll CQE 峰值
                std::atomic<std::uint64_t> m_enobufsCompletions{ 0 };   // provided-buffer 饥饿次数
                std::atomic<std::uint64_t> m_cqOverflowCount{ 0 };      // 最近内核 overflow 累计值
                std::atomic<std::uint64_t> m_receivedBytes{ 0 };        // 成功 read CQE payload 总字节数
                std::atomic<std::uint64_t> m_receiveCompletions{ 0 };   // 成功 provided-buffer read CQE 数
                std::atomic<std::uint64_t> m_receiveBundleCompletions{ 0 }; // bundle 语义成功 CQE 数
                std::atomic<std::uint64_t> m_receiveBundleBuffers{ 0 }; // bundle 累计消费 buffer 数
                std::atomic<std::uint64_t> m_maximumReceiveBundleBuffers{ 0 }; // 单 CQE buffer 峰值
                std::atomic<std::uint64_t> m_datagramReceiveCompletions{ 0 }; // 成功 recvmsg CQE 数
                std::atomic<std::uint64_t> m_datagramSendCompletions{ 0 }; // 成功 sendmsg CQE 数
                std::atomic<std::uint64_t> m_receivedDatagrams{ 0 }; // 已交付业务的数据报数
                std::atomic<std::uint64_t> m_sentDatagrams{ 0 }; // 已完成发送的数据报数
                std::atomic<std::uint64_t> m_zeroLengthDatagrams{ 0 }; // 收发空数据报数
                std::atomic<std::uint64_t> m_truncatedDatagrams{ 0 }; // 接收截断数据报数
                std::atomic<std::size_t> m_pendingDatagramSends{ 0 }; // 等待 sendmsg CQE 的数据报数
                std::atomic<std::uint64_t> m_datagramMultishotReceiveCompletions{ 0 }; // multishot UDP CQE 数
                std::atomic<std::uint64_t> m_datagramReceiveFallbacks{ 0 }; // 能力错误回退次数
                std::atomic<std::uint64_t> m_datagramSendBatchSubmissions{ 0 }; // 有效发送批次数
                std::atomic<std::size_t> m_maximumDatagramSendBatch{ 0 }; // 单批发送峰值
                std::atomic<std::uint64_t> m_bufferReconfigurationCount{ 0 }; // 成功切换 generation 次数
                std::atomic<std::uint64_t> m_bufferPolicyEvaluationCount{ 0 }; // 已处理反馈窗口数
                std::atomic<std::uint64_t> m_bufferPolicyRecommendationCount{ 0 }; // 已产生候选窗口数
                ProvidedBufferPolicy m_bufferPolicy;                    // 单 Poller 自适应迟滞状态
                std::uint64_t m_lastPolicyReceiveCompletions = 0;       // 上次窗口 read CQE 累计值
                std::uint64_t m_lastPolicyReceivedBytes = 0;            // 上次窗口 payload 累计值
                std::uint64_t m_lastPolicyEnobufsCompletions = 0;       // 上次窗口 ENOBUFS 累计值
                std::uint64_t m_lastPolicyCqOverflowCount = 0;          // 上次窗口 CQ overflow 累计值
                unsigned int m_ringEntries = 0;                         // 运行时自适应 SQ 容量
                std::atomic<unsigned int> m_completionBudget{ 0 };      // 反馈调节的单轮 CQE 基线预算
                unsigned int m_lowBatchStreak = 0;                      // 连续低利用率批次，提供缩减迟滞
                std::uint16_t m_nextBufferGroup = kFirstProvidedBufferGroup; // 下一候选 16 位 group id
                bool m_initialized = false;                             // ring 与 buffer group 已创建
                bool m_activated = false;                               // single issuer 线程已启用 ring
                bool m_receiveBundleFeatureEnabled = false;             // 内核 feature 支持时允许各 generation bundle
                std::atomic<bool> m_datagramMultishotEnabled{ false };   // UDP multishot 首次成功启用后锁存
                bool m_datagramMultishotDisabled = false;               // 能力错误后本 Poller 不再重复试探
            };

            IoUringPoller::IoUringPoller(EventLoop* ownerLoop)
                : Poller(ownerLoop),
                m_impl(new IoUringPollerImpl{}) {
                m_impl->m_ringEntries = DetectRingEntries(); // CPU 规格变化时自动调整有界队列容量
                const std::uint32_t initialBufferCount = DetectProvidedBufferCount(
                    m_impl->m_ringEntries); // 初始 generation 随 SQ 规格有界缩放
                m_impl->m_completionBudget = initialBufferCount; // 与可并行接收批次保持同一有界尺度

                io_uring_params parameters{}; // single issuer 由 EventLoop::Start 线程启用
                parameters.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_R_DISABLED;
#ifdef IORING_SETUP_COOP_TASKRUN
                parameters.flags |= IORING_SETUP_COOP_TASKRUN;
#endif
#ifdef IORING_SETUP_DEFER_TASKRUN
                // completion task-work 延迟到 issuer 进入 ring 时批量执行，减少跨 CPU 唤醒。
                parameters.flags |= IORING_SETUP_DEFER_TASKRUN;
#endif
#ifdef IORING_SETUP_NO_SQARRAY
                // 现代内核按 SQE 索引直接消费提交项，减少一层 SQ array 缓存访问。
                parameters.flags |= IORING_SETUP_NO_SQARRAY;
#endif
                const int initializeResult = io_uring_queue_init_params(
                    m_impl->m_ringEntries,
                    &m_impl->m_ring,
                    &parameters);
                if (initializeResult < 0) {
                    SetLastError(-initializeResult);
                    return;
                }
#ifdef IORING_FEAT_RECVSEND_BUNDLE
                m_impl->m_receiveBundleFeatureEnabled =
                    (parameters.features & IORING_FEAT_RECVSEND_BUNDLE) != 0; // 不按版本号猜测能力
#endif

                int generationError = 0; // 初始 group 也复用重配置的注册与填充路径
                try {
                    m_impl->m_currentBufferGeneration = m_impl->CreateBufferGeneration(
                        kProvidedBufferSize,
                        initialBufferCount,
                        generationError);
                }
                catch (...) {
                    generationError = ENOMEM;
                }
                if (!m_impl->m_currentBufferGeneration) {
                    SetLastError(generationError != 0 ? generationError : ENOMEM);
                    io_uring_queue_exit(&m_impl->m_ring);
                    return;
                }

                // UDP group 是可选优化；注册失败时继续保留完整 one-shot UDP 行为。
                const std::size_t datagramBufferSize = DatagramProvidedBufferSize(
                    sizeof(io_uring_recvmsg_out),
                    sizeof(sockaddr_storage),
                    64 * 1024);
                const std::uint32_t datagramBufferCount = DatagramProvidedBufferCount(
                    m_impl->m_ringEntries);
                int datagramGenerationError = 0; // 仅用于诊断可选资源失败，不覆盖 TCP 初始化错误
                try {
                    m_impl->m_datagramBufferGeneration = m_impl->CreateDatagramBufferGeneration(
                        datagramBufferSize,
                        datagramBufferCount,
                        datagramGenerationError);
                }
                catch (...) {
                    m_impl->m_datagramBufferGeneration.reset();
                }
                m_impl->m_initialized = true;
            }

            IoUringPoller::~IoUringPoller() {
                if (m_impl == nullptr) return;

                // connect socket 尚未转交回调时仍由 Poller 负责关闭。
                for (auto& item : m_impl->m_connectStates) {
                    if (item.second) Internal::CloseSocket(item.second->m_fd);
                }
                m_impl->m_connectStates.clear();

                // ConnectionState 可能持有最后一个连接引用，必须在 queue_exit 前解除回调并释放。
                for (auto& item : m_impl->m_connectionStates) {
                    if (item.second && item.second->m_connection) {
                        PollerAccess::Detach(*item.second->m_connection);
                    }
                }
                m_impl->m_connectionStates.clear();

                // EventLoop 已先停止连接；先注销全部 group，失败项由 queue_exit 兜底清理。
                if (m_impl->m_datagramBufferGeneration) {
                    (void)m_impl->UnregisterDatagramBufferGeneration(
                        *m_impl->m_datagramBufferGeneration);
                }
                if (m_impl->m_currentBufferGeneration) {
                    (void)m_impl->UnregisterBufferGeneration(*m_impl->m_currentBufferGeneration);
                }
                for (const auto& generation : m_impl->m_retiringBufferGenerations) {
                    if (generation) (void)m_impl->UnregisterBufferGeneration(*generation);
                }
                for (const auto& generation : m_impl->m_retainedBufferGenerations) {
                    if (generation) (void)m_impl->UnregisterBufferGeneration(*generation);
                }
                if (m_impl->m_initialized) io_uring_queue_exit(&m_impl->m_ring);
                if (m_impl->m_datagramBufferGeneration) {
                    m_impl->CloseDatagramBufferGenerationPool(
                        *m_impl->m_datagramBufferGeneration);
                    m_impl->m_datagramBufferGeneration.reset();
                }
                if (m_impl->m_currentBufferGeneration) {
                    m_impl->CloseBufferGenerationPool(*m_impl->m_currentBufferGeneration);
                    m_impl->m_currentBufferGeneration.reset();
                }
                for (const auto& generation : m_impl->m_retiringBufferGenerations) {
                    if (generation) m_impl->CloseBufferGenerationPool(*generation);
                }
                m_impl->m_retiringBufferGenerations.clear();
                for (const auto& generation : m_impl->m_retainedBufferGenerations) {
                    if (generation) m_impl->CloseBufferGenerationPool(*generation);
                }
                m_impl->m_retainedBufferGenerations.clear();
                delete m_impl;
                m_impl = nullptr;
            }

            bool IoUringPoller::IsReady() const noexcept {
                return m_impl != nullptr && m_impl->m_initialized;
            }

            bool IoUringPoller::Activate() {
                if (m_impl == nullptr || !m_impl->m_initialized) return false;
                if (m_impl->m_activated) return true;

                const int result = io_uring_enable_rings(&m_impl->m_ring); // 当前线程成为 single issuer
                if (result < 0) {
                    SetLastError(-result);
                    return false;
                }
                m_impl->m_activated = true;
                return true;
            }

            bool IoUringPoller::AddChannel(Channel* channel) {
                if (m_impl == nullptr || channel == nullptr || channel->GetSocket() == kInvalidSocket) return false;
                if (m_impl->m_channelStates.find(channel) != m_impl->m_channelStates.end()) return UpdateChannel(channel);
                if (!StoreChannel(channel)) return false;

                auto state = std::make_unique<IoUringPollerImpl::ChannelState>(channel); // CQE 稳定状态
                state->m_events = channel->Events();
                IoUringPollerImpl::ChannelState* rawState = state.get();
                m_impl->m_channelStates.emplace(channel, std::move(state));
                m_impl->QueueChannel(*rawState);
                channel->SetIndex(Channel::Index::Added);
                return true;
            }

            bool IoUringPoller::RemoveChannel(Channel* channel) {
                if (m_impl == nullptr || channel == nullptr) return false;
                const auto found = m_impl->m_channelStates.find(channel);
                if (found == m_impl->m_channelStates.end()) return false;

                IoUringPollerImpl::ChannelState& state = *found->second; // 取消完成前保留标签地址
                state.m_closing = true;
                state.m_events = IOEvent::None;
                state.m_cancelPending = state.m_outstanding;
                if (state.m_outstanding && !state.m_cancelRequested) {
                    state.m_cancelRequested = m_impl->Cancel(state.m_operation);
                }
                (void)EraseChannel(channel);
                channel->SetIndex(Channel::Index::Deleted);
                if (!state.m_outstanding && !state.m_queued) m_impl->m_channelStates.erase(found);
                return true;
            }

            bool IoUringPoller::UpdateChannel(Channel* channel) {
                if (m_impl == nullptr || channel == nullptr) return false;
                const auto found = m_impl->m_channelStates.find(channel);
                if (found == m_impl->m_channelStates.end()) return AddChannel(channel);

                IoUringPollerImpl::ChannelState& state = *found->second; // 先保存下一代关注集合
                state.m_events = channel->Events();
                state.m_cancelPending = state.m_outstanding;
                if (state.m_outstanding && !state.m_cancelRequested) {
                    state.m_cancelRequested = m_impl->Cancel(state.m_operation);
                }
                else if (!state.m_outstanding) {
                    m_impl->QueueChannel(state);
                }
                return true;
            }

            void IoUringPoller::Poll(int timeoutMs, std::vector<Channel*>& active) {
                active.clear();
                if (m_impl == nullptr || !m_impl->m_activated) return;

                Flush(); // 等待前一次性发布 listener、wakeup、recv 与 send 操作
                if (timeoutMs > 0) {
                    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(timeoutMs); // EINTR 重试共享调用者的原始等待上界
                    int waitResult = -ETIME; // 到达 deadline 与内核 timeout 使用同一正常退出语义
                    while (true) {
                        const auto now = std::chrono::steady_clock::now(); // 每次信号中断后重算剩余预算
                        if (now >= deadline) break;

                        const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            deadline - now);
                        const auto remainingSeconds = std::chrono::duration_cast<std::chrono::seconds>(
                            remaining);
                        __kernel_timespec timeout{}; // io_uring 接收相对时间，不能在重试时复用原值
                        timeout.tv_sec = remainingSeconds.count();
                        timeout.tv_nsec = (remaining - remainingSeconds).count();
                        io_uring_cqe* first = nullptr; // wait 只保证至少一个 CQE，批处理在后续 peek 完成
                        waitResult = io_uring_wait_cqe_timeout(&m_impl->m_ring, &first, &timeout);
                        if (waitResult != -EINTR) break;
                    }
                    if (waitResult < 0 && waitResult != -ETIME && waitResult != -EINTR) {
                        SetLastError(-waitResult);
                    }
                }

                std::array<io_uring_cqe*, 256> completions{}; // 单轮批量消费 CQE，减少 ring head 更新
                const unsigned int completionBudget = m_impl->CurrentCompletionBudget(); // 按当前连接压力自适应
                unsigned int processed = 0; // 达到预算后先返回 EventLoop 提交本批响应与任务
                while (processed < completionBudget) {
                    const unsigned int remaining = completionBudget - processed; // 本轮剩余公平预算
                    const unsigned int batchCapacity = std::min(
                        remaining,
                        static_cast<unsigned int>(completions.size()));
                    const unsigned int count = io_uring_peek_batch_cqe(
                        &m_impl->m_ring,
                        completions.data(),
                        batchCapacity);
                    if (count == 0) break;

                    for (unsigned int index = 0; index < count; ++index) {
                        io_uring_cqe* completion = completions[index]; // 当前平台完成项
                        auto* operation = static_cast<IoUringPollerImpl::Operation*>(
                            io_uring_cqe_get_data(completion));
                        if (operation == nullptr || operation->m_state == nullptr) continue;

                        if (operation->m_type == IoUringPollerImpl::OperationType::Channel) {
                            auto* state = static_cast<IoUringPollerImpl::ChannelState*>(operation->m_state);
                            state->m_outstanding = false;
                            state->m_cancelRequested = false;
                            state->m_cancelPending = false;
                            if (!state->m_closing && state->m_channel != nullptr) {
                                const IOEvent events = completion->res >= 0
                                    ? FromPollMask(completion->res)
                                    : (completion->res == -ECANCELED ? IOEvent::None : IOEvent::Error);
                                if (events != IOEvent::None) {
                                    state->m_channel->SetRevents(events);
                                    active.push_back(state->m_channel);
                                }
                                m_impl->QueueChannel(*state);
                            }
                            else if (!state->m_queued) {
                                m_impl->m_channelStates.erase(state->m_channel);
                            }
                            continue;
                        }

                        if (operation->m_type == IoUringPollerImpl::OperationType::Accept) {
                            auto* state = static_cast<IoUringPollerImpl::AcceptState*>(operation->m_state);
                            const SocketType key = state->m_fd; // 回调与取消期间保持 map 定位 key
                            const bool hasMore = (completion->flags & IORING_CQE_F_MORE) != 0;
                            state->m_outstanding = hasMore;
                            state->m_cancelRequested = RetainCancellationRequest(
                                state->m_cancelRequested,
                                hasMore); // MORE 到 terminal 之间不得重复提交 cancel
                            state->m_dispatching = true;

                            if (completion->res >= 0) {
                                const SocketType clientFd = static_cast<SocketType>(completion->res); // 新连接 fd
                                if (state->m_closing || !state->m_callback) {
                                    (void)::close(clientFd);
                                }
                                else {
                                    try {
                                        state->m_callback(clientFd);
                                    }
                                    catch (...) {
                                        // 用户 accept 消费入口失败时关闭 fd，避免 completion 路径泄漏。
                                        (void)::close(clientFd);
                                    }
                                }
                            }
                            state->m_dispatching = false;

                            if (!state->m_outstanding) {
                                if (state->m_closing) m_impl->m_acceptStates.erase(key);
                                else (void)m_impl->SubmitAccept(*state);
                            }
                            continue;
                        }

                        if (operation->m_type == IoUringPollerImpl::OperationType::Timeout) {
                            auto* state = static_cast<IoUringPollerImpl::TimeoutState*>(operation->m_state);
                            const TimeoutId timeoutId = state->m_timeoutId; // callback 可能请求取消同一个 timer
                            state->m_outstanding = false;
                            state->m_cancelRequested = false;
                            state->m_dispatching = true;
                            if (!state->m_closing && completion->res == -ETIME && state->m_callback) {
                                try {
                                    state->m_callback();
                                }
                                catch (...) {
                                    // timer 回调异常不能越过 EventLoop completion 分派边界。
                                }
                            }
                            state->m_dispatching = false;
                            m_impl->m_timeoutStates.erase(timeoutId);
                            continue;
                        }

                        if (operation->m_type == IoUringPollerImpl::OperationType::Connect) {
                            auto* state = static_cast<IoUringPollerImpl::ConnectState*>(operation->m_state);
                            const ConnectId connectId = state->m_connectId; // callback 可触发同一 id 的取消请求
                            state->m_outstanding = false;
                            state->m_cancelRequested = false;
                            state->m_dispatching = true;

                            if (!state->m_closing && state->m_callback) {
                                const int error = completion->res < 0 ? -completion->res : 0; // CQE 负值转 errno
                                SocketType completedFd = state->m_fd; // 成功时把唯一关闭责任移交 callback
                                if (error == 0) state->m_fd = kInvalidSocket;
                                try {
                                    state->m_callback(error == 0 ? completedFd : kInvalidSocket, error);
                                }
                                catch (...) {
                                    if (error == 0) Internal::CloseSocket(completedFd);
                                }
                            }
                            state->m_dispatching = false;
                            Internal::CloseSocket(state->m_fd); // 失败、取消或未消费回调统一关闭
                            state->m_fd = kInvalidSocket;
                            m_impl->m_pendingConnectOperations.fetch_sub(1, std::memory_order_relaxed);
                            m_impl->m_connectStates.erase(connectId);
                            continue;
                        }

                        if (operation->m_type == IoUringPollerImpl::OperationType::DatagramWrite) {
                            auto* datagram = static_cast<IoUringPollerImpl::DatagramWriteState*>(
                                operation->m_state); // operation 标签直接定位地址稳定节点
                            auto* state = datagram != nullptr ? datagram->m_owner : nullptr;
                            if (state == nullptr || state->m_connection == nullptr) continue;

                            Connection* key = state->m_connection.get(); // Error/close 可能摘除连接
                            state->m_dispatching = true;
                            datagram->m_completed = true;
                            datagram->m_completionResult = completion->res;
                            if (state->m_datagramWriteOutstanding > 0) {
                                --state->m_datagramWriteOutstanding;
                            }
                            state->m_writeOutstanding = state->m_datagramWriteOutstanding != 0;
                            if (!state->m_writeOutstanding) state->m_writeCancelRequested = false;

                            int writeError = 0; // FIFO 头第一个真实错误在回收后统一通知
                            while (!state->m_datagramWrites.empty()
                                && state->m_datagramWrites.front().m_completed) {
                                IoUringPollerImpl::DatagramWriteState& head =
                                    state->m_datagramWrites.front();
                                const std::size_t payloadBytes = head.m_buffer.ReadableBytes();
                                const bool completed = head.m_completionResult >= 0
                                    && DatagramWriteCompleted(
                                        static_cast<std::size_t>(head.m_completionResult),
                                        payloadBytes);

                                if (head.m_counted) {
                                    head.m_counted = false; // pending 统计在成功、错误与关闭间只扣一次
                                    state->m_pendingWriteBytes -= std::min(
                                        head.m_queueCost,
                                        state->m_pendingWriteBytes);
                                    m_impl->m_pendingDatagramSends.fetch_sub(
                                        1,
                                        std::memory_order_relaxed);
                                }
                                if (!state->m_closing && completed) {
                                    m_impl->m_datagramSendCompletions.fetch_add(
                                        1,
                                        std::memory_order_relaxed);
                                    m_impl->m_sentDatagrams.fetch_add(1, std::memory_order_relaxed);
                                    if (payloadBytes == 0) {
                                        m_impl->m_zeroLengthDatagrams.fetch_add(
                                            1,
                                            std::memory_order_relaxed);
                                    }
                                    PollerAccess::WriteDrain(
                                        *state->m_connection,
                                        state->m_pendingWriteBytes);
                                }
                                else if (!state->m_closing
                                    && head.m_completionResult != -ECANCELED
                                    && !state->m_datagramWriteErrorReported) {
                                    state->m_datagramWriteErrorReported = true;
                                    writeError = head.m_completionResult < 0
                                        ? -head.m_completionResult
                                        : EIO;
                                }
                                PollerAccess::DatagramWriteCompleted(
                                    *state->m_connection,
                                    head.m_peer,
                                    head.m_queueCost); // 成功、错误与 cancel 都精确回收 peer 成本
                                state->m_datagramWrites.pop_front(); // 当前节点 CQE 已回收，地址可释放
                            }

                            if (writeError != 0) {
                                PollerAccess::Error(*state->m_connection, writeError);
                            }
                            if (!state->m_closing && state->m_datagramWriteOutstanding == 0) {
                                if (state->m_datagramWrites.empty()) {
                                    PollerAccess::WriteComplete(*state->m_connection);
                                }
                                else {
                                    (void)m_impl->SubmitWrite(*state); // 下一批等待下次 Flush 统一发布
                                }
                            }
                            state->m_dispatching = false;
                            if (state->m_closing && !state->m_readOutstanding
                                && state->m_datagramWriteOutstanding == 0) {
                                m_impl->m_connectionStates.erase(key);
                            }
                            continue;
                        }

                        auto* state = static_cast<IoUringPollerImpl::ConnectionState*>(operation->m_state);
                        Connection* key = state->m_connection.get(); // 回调可能关闭并摘除连接
                        state->m_dispatching = true;
                        if (operation->m_type == IoUringPollerImpl::OperationType::DatagramRead) {
                            const bool multishot = state->m_datagramMultishot; // terminal CQE 前保留路径身份
                            const bool hasMore = multishot
                                && (completion->flags & IORING_CQE_F_MORE) != 0;
                            state->m_readOutstanding = hasMore;
                            state->m_readCancelRequested = RetainCancellationRequest(
                                state->m_readCancelRequested,
                                hasMore);
                            if (!hasMore) state->m_datagramMultishot = false;

                            if (multishot) {
                                IoUringPollerImpl::DatagramBufferGeneration* generation =
                                    m_impl->m_datagramBufferGeneration.get(); // 固定 group 不参与 TCP 代切换
                                const bool hasBuffer =
                                    (completion->flags & IORING_CQE_F_BUFFER) != 0;
                                const unsigned int bufferId =
                                    completion->flags >> IORING_CQE_BUFFER_SHIFT;
                                bool delivered = false; // true 表示 token 已由业务 Buffer lease 接管
                                if (completion->res >= 0 && hasBuffer && generation != nullptr) {
                                    if (!state->m_closing && state->m_readEnabled
                                        && PollerAccess::ReadEnabled(*state->m_connection)) {
                                        delivered = m_impl->CompleteDatagramReceivedBuffer(
                                            *state,
                                            *generation,
                                            *completion);
                                        if (!delivered) {
                                            PollerAccess::Error(*state->m_connection, EPROTO);
                                        }
                                    }
                                    if (!delivered) {
                                        m_impl->RecycleDatagramBuffer(*generation, bufferId);
                                    }
                                }
                                else if (!state->m_closing
                                    && (completion->res == -EINVAL
                                        || completion->res == -EOPNOTSUPP
                                        || completion->res == -ENOSYS)) {
                                    if (!m_impl->m_datagramMultishotDisabled) {
                                        m_impl->m_datagramMultishotDisabled = true;
                                        m_impl->m_datagramMultishotEnabled.store(
                                            false,
                                            std::memory_order_relaxed);
                                        m_impl->m_datagramReceiveFallbacks.fetch_add(
                                            1,
                                            std::memory_order_relaxed);
                                    }
                                }
                                else if (!state->m_closing && completion->res == -ENOBUFS) {
                                    state->m_datagramReadWaitingForBuffer = true;
                                    m_impl->m_enobufsCompletions.fetch_add(
                                        1,
                                        std::memory_order_relaxed); // token 归还后重提同一快路径
                                }
                                else if (!state->m_closing
                                    && completion->res != -EAGAIN
                                    && completion->res != -EWOULDBLOCK
                                    && completion->res != -ECANCELED) {
                                    PollerAccess::Error(
                                        *state->m_connection,
                                        completion->res < 0 ? -completion->res : EPROTO);
                                }
                                else if (!state->m_closing && completion->res >= 0 && !hasBuffer) {
                                    PollerAccess::Error(*state->m_connection, EPROTO);
                                }
                            }
                            else if (!state->m_closing && completion->res >= 0) {
                                const auto received = InterpretDatagramReceive(
                                    static_cast<std::size_t>(completion->res),
                                    state->m_maxDatagramBytes,
                                    (state->m_datagramReadMessage.msg_flags & MSG_TRUNC) != 0);
                                state->m_datagramReadBuffer.HasWritten(received.payloadBytes);
                                Address peer(
                                    state->m_datagramPeer,
                                    static_cast<SocketLength>(state->m_datagramReadMessage.msg_namelen));
                                if (!peer.IsValid()) {
                                    PollerAccess::Error(*state->m_connection, EPROTO);
                                }
                                else {
                                    m_impl->m_datagramReceiveCompletions.fetch_add(
                                        1,
                                        std::memory_order_relaxed);
                                    m_impl->m_receivedDatagrams.fetch_add(1, std::memory_order_relaxed);
                                    if (received.originalBytes == 0) {
                                        m_impl->m_zeroLengthDatagrams.fetch_add(1, std::memory_order_relaxed);
                                    }
                                    if (received.truncated) {
                                        m_impl->m_truncatedDatagrams.fetch_add(1, std::memory_order_relaxed);
                                    }
                                    PollerAccess::CompleteDatagram(
                                        *state->m_connection,
                                        state->m_datagramReadBuffer,
                                        peer,
                                        received.originalBytes,
                                        received.truncated);
                                }
                                state->m_datagramReadBuffer.RetrieveAll(); // 下一次 recvmsg 复用固定容量
                            }
                            else if (!multishot && !state->m_closing
                                && completion->res != -EAGAIN
                                && completion->res != -EWOULDBLOCK
                                && completion->res != -ECANCELED) {
                                PollerAccess::Error(*state->m_connection, -completion->res);
                            }

                            if (ShouldResubmitDatagramRead(
                                    state->m_closing,
                                    state->m_readEnabled
                                        && PollerAccess::ReadEnabled(*state->m_connection),
                                    multishot && completion->res == -ENOBUFS)) {
                                (void)m_impl->SubmitRead(*state);
                            }
                            state->m_dispatching = false;
                            if (state->m_closing && !state->m_readOutstanding
                                && !state->m_writeOutstanding) {
                                m_impl->m_connectionStates.erase(key);
                            }
                            continue;
                        }
                        if (operation->m_type == IoUringPollerImpl::OperationType::Read) {
                            IoUringPollerImpl::ProvidedBufferGeneration* readGeneration =
                                state->m_readGeneration; // terminal CQE 前始终指向提交时的原 group
                            const bool hasMore = (completion->flags & IORING_CQE_F_MORE) != 0; // multishot 存活标记
                            state->m_readOutstanding = hasMore;
                            state->m_readCancelRequested = RetainCancellationRequest(
                                state->m_readCancelRequested,
                                hasMore); // cancel latch 随原 multishot terminal CQE 一起结束
                            if (!hasMore) {
                                state->m_readGeneration = nullptr;
                                if (readGeneration != nullptr && readGeneration->m_outstandingReads > 0) {
                                    --readGeneration->m_outstandingReads;
                                }
                            }

                            const bool hasBuffer = (completion->flags & IORING_CQE_F_BUFFER) != 0;
                            const unsigned int bufferId = completion->flags >> IORING_CQE_BUFFER_SHIFT; // 内核选择的 buffer id
                            if (completion->res > 0 && hasBuffer && readGeneration != nullptr) {
                                m_impl->m_receivedBytes.fetch_add(
                                    static_cast<std::uint64_t>(completion->res),
                                    std::memory_order_relaxed); // 策略窗口统计完整 CQE payload
                                m_impl->m_receiveCompletions.fetch_add(1, std::memory_order_relaxed);
                                m_impl->CompleteReceivedBuffers(*state, *readGeneration, *completion);
                            }
                            else if (!state->m_closing && completion->res == 0) {
                                PollerAccess::PeerClosed(*state->m_connection);
                            }
                            else if (!state->m_closing && completion->res == -ENOBUFS) {
                                // buffer ring 饥饿需要进入反馈快照，不作为连接级错误关闭。
                                m_impl->m_enobufsCompletions.fetch_add(1, std::memory_order_relaxed);
                            }
                            else if (!state->m_closing
                                && completion->res < 0
                                && completion->res != -EAGAIN
                                && completion->res != -EWOULDBLOCK
                                && completion->res != -ECANCELED
                                && completion->res != -ENOBUFS) {
                                PollerAccess::Error(*state->m_connection, -completion->res);
                            }
                            else if (!state->m_closing && completion->res > 0 && !hasBuffer) {
                                PollerAccess::Error(*state->m_connection, EPROTO);
                            }
                            else if (!state->m_closing && completion->res > 0 && readGeneration == nullptr) {
                                PollerAccess::Error(*state->m_connection, EPROTO);
                            }
                            if (hasBuffer && completion->res <= 0 && readGeneration != nullptr) {
                                // 错误/关闭 completion 最多消费一个 CQE 标记 buffer，立即归还。
                                ++readGeneration->m_cachedBufferHead;
                                m_impl->RecycleBuffer(*readGeneration, bufferId);
                            }

                            if (!state->m_closing && !state->m_readOutstanding
                                && state->m_readEnabled && PollerAccess::ReadEnabled(*state->m_connection)) {
                                (void)m_impl->SubmitRead(*state);
                            }
                            m_impl->RetireUnusedBufferGenerations(); // 所有旧 terminal CQE 回收后注销 group
                        }
                        else {
                            state->m_writeOutstanding = false;
                            state->m_writeCancelRequested = false;
                            if (!state->m_closing && completion->res > 0) {
                                const std::size_t completedBytes = static_cast<std::size_t>(completion->res); // 当前 send 完成量
                                const std::size_t appliedBytes = std::min(completedBytes, state->m_pendingWriteBytes);
                                state->m_pendingWriteBytes -= appliedBytes;
                                state->m_writeChain.Consume(appliedBytes); // 完成多少就释放多少链首所有权
                                PollerAccess::WriteDrain(*state->m_connection, state->m_pendingWriteBytes);
                                if (state->m_writeChain.Empty()) PollerAccess::WriteComplete(*state->m_connection);
                                else (void)m_impl->SubmitWrite(*state);
                            }
                            else if (!state->m_closing
                                && (completion->res == -EAGAIN || completion->res == -EWOULDBLOCK)) {
                                (void)m_impl->SubmitWrite(*state);
                            }
                            else if (!state->m_closing && completion->res != -ECANCELED) {
                                PollerAccess::Error(
                                    *state->m_connection,
                                    completion->res < 0 ? -completion->res : EIO);
                            }
                        }

                        state->m_dispatching = false;
                        if (state->m_closing && !state->m_readOutstanding && !state->m_writeOutstanding) {
                            m_impl->m_connectionStates.erase(key);
                        }
                    }

                    io_uring_cq_advance(&m_impl->m_ring, count);
                    m_impl->m_completedOperations.fetch_add(count, std::memory_order_relaxed);
                    processed += count;
                }
                m_impl->ObserveCompletionBatch(processed, completionBudget);
                const auto recommendation = m_impl->ObserveBufferPolicy(); // issuer 线程评估有界反馈窗口
                if (recommendation.changed) {
                    (void)ReconfigureProvidedBuffers(
                        recommendation.bufferSize,
                        recommendation.bufferCount); // 失败时当前 generation 保持完整
                }
            }

            void IoUringPoller::Flush() {
                if (m_impl == nullptr || !m_impl->m_initialized) return;
                m_impl->CancelPendingConnects(); // connect deadline/Shutdown 的取消失败必须持续重试
                m_impl->RetryPendingOperationCancellations(); // SQ 恢复后补齐暂停、关闭与切换 cancel
                m_impl->DrainReturnedBuffers(); // single issuer 批量回填任意线程释放的 provided buffers
                m_impl->RetryPendingDatagramOperations(); // UDP ENOBUFS 只在 token 回填后重新提交
                m_impl->RetireUnusedBufferGenerations(); // 无 outstanding read 的旧 ring 在提交前释放
                m_impl->CleanupRetainedBufferGenerations(); // 最后一个旧 lease 释放后回收存储
                m_impl->DrainChannelSubmissions();
                const int result = io_uring_submit(&m_impl->m_ring); // 单次 syscall 发布当前所有 SQE
                if (result < 0) SetLastError(-result);
            }

            Poller::ConnectId IoUringPoller::StartConnect(
                const Address& remoteAddress,
                ConnectCallback callback) noexcept {
                if (m_impl == nullptr || !m_impl->m_activated
                    || !remoteAddress.IsValid() || !callback) return InvalidConnectId;

                const SocketType fd = Internal::CreateSocket(
                    remoteAddress.FamilyValue(),
                    SOCK_STREAM,
                    IPPROTO_TCP); // socket 在成功 CQE 前由 ConnectState 独占
                if (fd == kInvalidSocket) {
                    SetLastError(Internal::GetLastSocketError());
                    return InvalidConnectId;
                }
                if (!Internal::SetNonBlocking(fd, true) || !Internal::SetTcpNoDelay(fd)) {
                    SetLastError(Internal::GetLastSocketError());
                    Internal::CloseSocket(fd);
                    return InvalidConnectId;
                }

                ConnectId connectId = m_impl->m_nextConnectId++; // issuer 线程独占分配 operation id
                while (connectId == InvalidConnectId
                    || m_impl->m_connectStates.find(connectId) != m_impl->m_connectStates.end()) {
                    connectId = m_impl->m_nextConnectId++;
                }

                IoUringPollerImpl::ConnectState* rawState = nullptr; // 入表后 sockaddr 与 operation 地址稳定
                try {
                    auto state = std::make_unique<IoUringPollerImpl::ConnectState>(
                        connectId,
                        fd,
                        remoteAddress,
                        std::move(callback));
                    const auto inserted = m_impl->m_connectStates.emplace(connectId, std::move(state));
                    if (!inserted.second) {
                        SetLastError(EEXIST);
                        Internal::CloseSocket(fd);
                        return InvalidConnectId;
                    }
                    rawState = inserted.first->second.get();
                }
                catch (...) {
                    SetLastError(ENOMEM);
                    Internal::CloseSocket(fd);
                    return InvalidConnectId;
                }

                io_uring_sqe* sqe = m_impl->AcquireSqe(); // connect 与本轮其他 operation 一起批量提交
                if (sqe == nullptr) {
                    m_impl->m_connectStates.erase(connectId);
                    Internal::CloseSocket(fd);
                    SetLastError(EBUSY);
                    return InvalidConnectId;
                }
                io_uring_prep_connect(
                    sqe,
                    static_cast<int>(fd),
                    rawState->m_remoteAddress.SockAddr(),
                    rawState->m_remoteAddress.Length());
                io_uring_sqe_set_data(sqe, &rawState->m_operation);
                rawState->m_outstanding = true;
                m_impl->m_connectSubmissions.fetch_add(1, std::memory_order_relaxed);
                m_impl->m_pendingConnectOperations.fetch_add(1, std::memory_order_relaxed);
                return connectId;
            }

            void IoUringPoller::CancelConnect(ConnectId connectId) noexcept {
                if (m_impl == nullptr || connectId == InvalidConnectId) return;
                const auto found = m_impl->m_connectStates.find(connectId);
                if (found == m_impl->m_connectStates.end()) return;

                IoUringPollerImpl::ConnectState& state = *found->second; // 原 CQE 回收前保持 operation 地址稳定
                state.m_closing = true;
                if (state.m_outstanding && !state.m_cancelRequested) {
                    state.m_cancelRequested = m_impl->SubmitConnectCancel(state);
                }
                if (!state.m_outstanding && !state.m_dispatching) {
                    Internal::CloseSocket(state.m_fd);
                    m_impl->m_connectStates.erase(found);
                }
            }

            bool IoUringPoller::StartConnection(const std::shared_ptr<Connection>& connection) {
                if (m_impl == nullptr || !m_impl->m_activated || !connection
                    || connection->GetSocket() == kInvalidSocket) return false;
                if (m_impl->m_connectionStates.find(connection.get()) != m_impl->m_connectionStates.end()) return true;

                auto state = std::make_unique<IoUringPollerImpl::ConnectionState>(connection); // CQE 稳定状态
                IoUringPollerImpl::ConnectionState* rawState = state.get();
                m_impl->m_connectionStates.emplace(connection.get(), std::move(state));
                if (!m_impl->SubmitRead(*rawState)) {
                    m_impl->m_connectionStates.erase(connection.get());
                    return false;
                }
                return true;
            }

            bool IoUringPoller::StartAccept(SocketType listenFd, AcceptCallback callback) {
                if (m_impl == nullptr || !m_impl->m_activated || listenFd == kInvalidSocket || !callback) return false;
                if (m_impl->m_acceptStates.find(listenFd) != m_impl->m_acceptStates.end()) return true;

                auto state = std::make_unique<IoUringPollerImpl::AcceptState>(
                    listenFd,
                    std::move(callback)); // callback 与 operation state 同生命周期
                IoUringPollerImpl::AcceptState* rawState = state.get();
                m_impl->m_acceptStates.emplace(listenFd, std::move(state));
                if (!m_impl->SubmitAccept(*rawState)) {
                    m_impl->m_acceptStates.erase(listenFd);
                    return false;
                }
                return true;
            }

            void IoUringPoller::StopAccept(SocketType listenFd) {
                if (m_impl == nullptr || listenFd == kInvalidSocket) return;
                const auto found = m_impl->m_acceptStates.find(listenFd);
                if (found == m_impl->m_acceptStates.end()) return;

                IoUringPollerImpl::AcceptState& state = *found->second; // completion 回收前保持 operation 地址稳定
                state.m_closing = true;
                if (state.m_outstanding && !state.m_cancelRequested) {
                    state.m_cancelRequested = m_impl->Cancel(state.m_operation);
                }
                if (!state.m_outstanding && !state.m_dispatching) m_impl->m_acceptStates.erase(found);
            }

            Poller::TimeoutId IoUringPoller::ScheduleTimeout(
                std::chrono::milliseconds delay,
                TimeoutCallback callback) noexcept {
                if (m_impl == nullptr || !m_impl->m_activated || !callback) return InvalidTimeoutId;

                TimeoutId timeoutId = m_impl->m_nextTimeoutId++; // issuer 线程独占分配 timer id
                while (timeoutId == InvalidTimeoutId
                    || m_impl->m_timeoutStates.find(timeoutId) != m_impl->m_timeoutStates.end()) {
                    timeoutId = m_impl->m_nextTimeoutId++;
                }

                IoUringPollerImpl::TimeoutState* rawState = nullptr; // 插入 map 后 operation 地址保持稳定
                try {
                    auto state = std::make_unique<IoUringPollerImpl::TimeoutState>(
                        timeoutId,
                        std::move(callback));
                    const auto inserted = m_impl->m_timeoutStates.emplace(
                        timeoutId,
                        std::move(state)); // 极端 id 回绕也必须确认 state 已进入稳定容器
                    if (!inserted.second) {
                        SetLastError(EEXIST);
                        return InvalidTimeoutId;
                    }
                    rawState = inserted.first->second.get();
                }
                catch (...) {
                    SetLastError(ENOMEM);
                    return InvalidTimeoutId;
                }

                const std::int64_t delayMs = std::max<std::int64_t>(0, delay.count()); // 负值按立即到期处理
                rawState->m_duration.tv_sec = delayMs / 1000;
                rawState->m_duration.tv_nsec = static_cast<long long>(delayMs % 1000) * 1000000LL;
                if (delayMs == 0) rawState->m_duration.tv_nsec = 1; // 避免零 timespec 被内核解释为非法输入

                io_uring_sqe* sqe = m_impl->AcquireSqe(); // 当前 timer operation 的提交项
                if (sqe == nullptr) {
                    m_impl->m_timeoutStates.erase(timeoutId);
                    SetLastError(EBUSY);
                    return InvalidTimeoutId;
                }

                io_uring_prep_timeout(sqe, &rawState->m_duration, 0, 0);
                io_uring_sqe_set_data(sqe, &rawState->m_operation);
                rawState->m_outstanding = true;
                return timeoutId;
            }

            void IoUringPoller::CancelTimeout(TimeoutId timeoutId) noexcept {
                if (m_impl == nullptr || timeoutId == InvalidTimeoutId) return;
                const auto found = m_impl->m_timeoutStates.find(timeoutId);
                if (found == m_impl->m_timeoutStates.end()) return;

                IoUringPollerImpl::TimeoutState& state = *found->second; // 原 CQE 回收前保持 operation 地址稳定
                state.m_closing = true;
                if (state.m_outstanding && !state.m_cancelRequested) {
                    state.m_cancelRequested = m_impl->Cancel(state.m_operation);
                }
                if (!state.m_outstanding && !state.m_dispatching) {
                    m_impl->m_timeoutStates.erase(found);
                }
            }

            bool IoUringPoller::QueueWrite(Connection* connection, Buffer&& buffer) {
                BufferChain chain; // 单 Buffer 入口统一收敛到多段写所有权链
                chain.Append(std::move(buffer));
                return QueueWrite(connection, std::move(chain));
            }

            bool IoUringPoller::QueueWrite(Connection* connection, BufferChain&& chain) {
                if (m_impl == nullptr || connection == nullptr) return false;
                const auto found = m_impl->m_connectionStates.find(connection);
                if (found == m_impl->m_connectionStates.end()) return false;

                IoUringPollerImpl::ConnectionState& state = *found->second; // loop 线程独占写状态
                const std::size_t len = chain.ReadableBytes(); // 移动前保存背压统计字节数
                if (state.m_closing || len == 0) return true;
                if (len > (std::numeric_limits<std::size_t>::max)() - state.m_pendingWriteBytes) {
                    PollerAccess::Error(*state.m_connection, ENOBUFS);
                    return true;
                }

                try {
                    state.m_writeChain.Append(std::move(chain)); // 跨链只移动 Buffer PImpl，不复制 payload
                }
                catch (...) {
                    PollerAccess::Error(*state.m_connection, ENOMEM);
                    return true;
                }
                state.m_pendingWriteBytes += len;
                if (!PollerAccess::WriteGrowth(*state.m_connection, state.m_pendingWriteBytes) || state.m_closing) return true;

                if (!state.m_writeOutstanding && !m_impl->SubmitWrite(state)) {
                    PollerAccess::Error(*state.m_connection, EIO);
                }
                return true;
            }

            bool IoUringPoller::QueueDatagramWrite(
                Connection* connection,
                const Address& peer,
                Buffer&& buffer) {
                if (m_impl == nullptr || connection == nullptr) return false;
                const auto found = m_impl->m_connectionStates.find(connection);
                if (found == m_impl->m_connectionStates.end()) return false;

                IoUringPollerImpl::ConnectionState& state = *found->second; // issuer 独占 UDP FIFO
                if (!state.m_datagram) return false;
                if (state.m_closing) return true;
                const DatagramWriteTarget target = ResolveDatagramWriteTarget(
                    connection->GetRemoteAddress().IsValid(),
                    peer.IsValid());
                if (target == DatagramWriteTarget::Invalid) return false;

                const std::size_t queueCost = DatagramQueueCost(buffer.ReadableBytes());
                if (queueCost > (std::numeric_limits<std::size_t>::max)()
                    - state.m_pendingWriteBytes) {
                    PollerAccess::Error(*state.m_connection, ENOBUFS);
                    return true;
                }

                try {
                    state.m_datagramWrites.emplace_back(&state, target, peer, std::move(buffer));
                }
                catch (...) {
                    PollerAccess::Error(*state.m_connection, ENOMEM);
                    return true;
                }
                state.m_pendingWriteBytes += queueCost;
                m_impl->m_pendingDatagramSends.fetch_add(1, std::memory_order_relaxed);
                if (!PollerAccess::WriteGrowth(*state.m_connection, state.m_pendingWriteBytes)) {
                    return true;
                }

                if (!state.m_writeOutstanding) (void)m_impl->SubmitWrite(state);
                return true;
            }

            void IoUringPoller::SetReadEnabled(Connection* connection, bool enabled) {
                if (m_impl == nullptr || connection == nullptr) return;
                const auto found = m_impl->m_connectionStates.find(connection);
                if (found == m_impl->m_connectionStates.end()) return;

                IoUringPollerImpl::ConnectionState& state = *found->second; // 业务背压状态
                state.m_readEnabled = enabled;
                if (!enabled && state.m_readOutstanding && !state.m_readCancelRequested) {
                    state.m_readCancelRequested = m_impl->Cancel(state.m_readOperation);
                }
                else if (enabled && !state.m_readOutstanding && !state.m_closing) {
                    (void)m_impl->SubmitRead(state);
                }
            }

            std::size_t IoUringPoller::PendingWriteBytes(const Connection* connection) const noexcept {
                if (m_impl == nullptr || connection == nullptr) return 0;
                const auto found = m_impl->m_connectionStates.find(const_cast<Connection*>(connection));
                return found == m_impl->m_connectionStates.end() ? 0 : found->second->m_pendingWriteBytes;
            }

            void IoUringPoller::StopConnection(Connection* connection) {
                if (m_impl == nullptr || connection == nullptr) return;
                const auto found = m_impl->m_connectionStates.find(connection);
                if (found == m_impl->m_connectionStates.end()) return;

                IoUringPollerImpl::ConnectionState& state = *found->second; // completion 回收前保留队列内存
                state.m_closing = true;
                state.m_readEnabled = false;
                if (state.m_datagram && !state.m_datagramWrites.empty()) {
                    std::size_t queuedDatagrams = 0; // 只扣仍计入 pending 的节点
                    for (auto& datagram : state.m_datagramWrites) {
                        if (datagram.m_counted) {
                            datagram.m_counted = false;
                            ++queuedDatagrams;
                        }
                    }
                    m_impl->m_pendingDatagramSends.fetch_sub(
                        queuedDatagrams,
                        std::memory_order_relaxed);
                    state.m_pendingWriteBytes = 0;
                    auto datagram = state.m_datagramWrites.begin(); // 未提交节点可立即释放所有权
                    while (datagram != state.m_datagramWrites.end()) {
                        if (!datagram->m_submitted) {
                            PollerAccess::DatagramWriteCompleted(
                                *state.m_connection,
                                datagram->m_peer,
                                datagram->m_queueCost); // 未发布 SQE 也按原 peer 只回收一次
                            datagram = state.m_datagramWrites.erase(datagram);
                        }
                        else {
                            ++datagram;
                        }
                    }
                    state.m_writeOutstanding = state.m_datagramWriteOutstanding != 0;
                }
                if (state.m_readOutstanding && !state.m_readCancelRequested) {
                    state.m_readCancelRequested = m_impl->Cancel(state.m_readOperation);
                }
                if (state.m_writeOutstanding && !state.m_writeCancelRequested) {
                    if (state.m_datagram) {
                        IoUringPollerImpl::DatagramWriteState* datagram =
                            m_impl->FirstIncompleteDatagramWrite(state);
                        if (datagram != nullptr) {
                            state.m_writeCancelRequested = m_impl->Cancel(
                                datagram->m_operation); // linked 后继通过 ECANCELED 自行回收
                        }
                    }
                    else {
                        state.m_writeCancelRequested = m_impl->Cancel(state.m_writeOperation);
                    }
                }
                if (!state.m_readOutstanding && !state.m_writeOutstanding && !state.m_dispatching) {
                    m_impl->m_connectionStates.erase(found);
                }
            }

            bool IoUringPoller::HasPendingShutdownCompletions() const noexcept {
                if (m_impl == nullptr) return false;
                for (const auto& item : m_impl->m_connectionStates) {
                    const IoUringPollerImpl::ConnectionState& state = *item.second;
                    if (state.m_closing && (state.m_readOutstanding || state.m_writeOutstanding)) {
                        return true;
                    }
                }
                for (const auto& item : m_impl->m_connectStates) {
                    const IoUringPollerImpl::ConnectState& state = *item.second;
                    if (state.m_closing && state.m_outstanding) return true;
                }
                for (const auto& item : m_impl->m_acceptStates) {
                    const IoUringPollerImpl::AcceptState& state = *item.second;
                    if (state.m_closing && state.m_outstanding) return true;
                }
                for (const auto& item : m_impl->m_timeoutStates) {
                    const IoUringPollerImpl::TimeoutState& state = *item.second;
                    if (state.m_closing && state.m_outstanding) return true;
                }
                return false;
            }

            std::uint64_t IoUringPoller::CompletedOperationCount() const noexcept {
                return m_impl ? m_impl->m_completedOperations.load(std::memory_order_relaxed) : 0;
            }

            std::size_t IoUringPoller::ProvidedBufferCount() const noexcept {
                if (m_impl == nullptr || !m_impl->m_initialized) return 0;
                std::lock_guard<std::mutex> lock(m_impl->m_bufferGenerationMutex); // 与切换指针串行
                return m_impl->m_currentBufferGeneration
                    ? m_impl->m_currentBufferGeneration->m_bufferCount
                    : 0;
            }

            std::uint64_t IoUringPoller::ReadSubmissionCount() const noexcept {
                return m_impl ? m_impl->m_readSubmissions.load(std::memory_order_relaxed) : 0;
            }

            std::uint64_t IoUringPoller::AcceptSubmissionCount() const noexcept {
                return m_impl ? m_impl->m_acceptSubmissions.load(std::memory_order_relaxed) : 0;
            }

            bool IoUringPoller::ReceiveBundleEnabled() const noexcept {
                if (m_impl == nullptr || !m_impl->m_initialized) return false;
                std::lock_guard<std::mutex> lock(m_impl->m_bufferGenerationMutex); // 当前代能力快照
                return m_impl->m_currentBufferGeneration
                    && m_impl->m_currentBufferGeneration->m_receiveBundleEnabled;
            }

            CompletionStats IoUringPoller::GetCompletionStats() const noexcept {
                CompletionStats stats; // 对外只复制原子与只读配置快照
                if (m_impl == nullptr || !m_impl->m_initialized) return stats;

                stats.completedOperations = m_impl->m_completedOperations.load(std::memory_order_relaxed);
                stats.connectSubmissions = m_impl->m_connectSubmissions.load(std::memory_order_relaxed);
                stats.connectCancelSubmissions =
                    m_impl->m_connectCancelSubmissions.load(std::memory_order_relaxed);
                stats.pendingConnectOperations =
                    m_impl->m_pendingConnectOperations.load(std::memory_order_relaxed);
                stats.completionBatchCount = m_impl->m_completionBatchCount.load(std::memory_order_relaxed);
                stats.completionBatchItems = m_impl->m_completionBatchItems.load(std::memory_order_relaxed);
                stats.peakCompletionBatch = m_impl->m_peakCompletionBatch.load(std::memory_order_relaxed);
                stats.enobufsCompletions = m_impl->m_enobufsCompletions.load(std::memory_order_relaxed);
                stats.cqOverflowCount = m_impl->m_cqOverflowCount.load(std::memory_order_relaxed);
                stats.receivedBytes = m_impl->m_receivedBytes.load(std::memory_order_relaxed);
                stats.receiveCompletions = m_impl->m_receiveCompletions.load(std::memory_order_relaxed);
                stats.receiveBundleCompletions =
                    m_impl->m_receiveBundleCompletions.load(std::memory_order_relaxed);
                stats.receiveBundleBuffers =
                    m_impl->m_receiveBundleBuffers.load(std::memory_order_relaxed);
                stats.maximumReceiveBundleBuffers =
                    m_impl->m_maximumReceiveBundleBuffers.load(std::memory_order_relaxed);
                stats.datagramReceiveCompletions =
                    m_impl->m_datagramReceiveCompletions.load(std::memory_order_relaxed);
                stats.datagramSendCompletions =
                    m_impl->m_datagramSendCompletions.load(std::memory_order_relaxed);
                stats.receivedDatagrams =
                    m_impl->m_receivedDatagrams.load(std::memory_order_relaxed);
                stats.sentDatagrams = m_impl->m_sentDatagrams.load(std::memory_order_relaxed);
                stats.zeroLengthDatagrams =
                    m_impl->m_zeroLengthDatagrams.load(std::memory_order_relaxed);
                stats.truncatedDatagrams =
                    m_impl->m_truncatedDatagrams.load(std::memory_order_relaxed);
                stats.pendingDatagramSends =
                    m_impl->m_pendingDatagramSends.load(std::memory_order_relaxed);
                stats.datagramMultishotEnabled =
                    m_impl->m_datagramMultishotEnabled.load(std::memory_order_relaxed);
                stats.datagramMultishotReceiveCompletions =
                    m_impl->m_datagramMultishotReceiveCompletions.load(std::memory_order_relaxed);
                stats.datagramReceiveFallbacks =
                    m_impl->m_datagramReceiveFallbacks.load(std::memory_order_relaxed);
                stats.datagramSendBatchSubmissions =
                    m_impl->m_datagramSendBatchSubmissions.load(std::memory_order_relaxed);
                stats.maximumDatagramSendBatch =
                    m_impl->m_maximumDatagramSendBatch.load(std::memory_order_relaxed);
                stats.completionBudget = m_impl->m_completionBudget.load(std::memory_order_relaxed);
                stats.providedBufferReconfigurationCount =
                    m_impl->m_bufferReconfigurationCount.load(std::memory_order_relaxed);
                stats.providedBufferPolicyEvaluationCount =
                    m_impl->m_bufferPolicyEvaluationCount.load(std::memory_order_relaxed);
                stats.providedBufferPolicyRecommendationCount =
                    m_impl->m_bufferPolicyRecommendationCount.load(std::memory_order_relaxed);

                std::lock_guard<std::mutex> lock(m_impl->m_bufferGenerationMutex); // pool 关闭前完成一致快照
                const auto* datagramGeneration = m_impl->m_datagramBufferGeneration.get(); // UDP 固定规格
                if (datagramGeneration != nullptr && datagramGeneration->m_bufferPool != nullptr) {
                    stats.datagramProvidedBufferCount =
                        datagramGeneration->m_bufferPool->BufferCount();
                    stats.datagramProvidedBufferSize =
                        datagramGeneration->m_bufferPool->BufferSize();
                    stats.datagramActiveBufferLeases =
                        datagramGeneration->m_bufferPool->ActiveLeaseCount();
                    stats.datagramPendingBufferReturns =
                        datagramGeneration->m_bufferPool->PendingReturnCount();
                    stats.datagramCurrentAvailableBuffers =
                        datagramGeneration->m_bufferPool->CurrentAvailableCount();
                }
                stats.retiringProvidedBufferGenerations =
                    m_impl->m_retiringBufferGenerations.size();
                stats.retainedProvidedBufferGenerations =
                    m_impl->m_retainedBufferGenerations.size();
                for (const auto& retained : m_impl->m_retainedBufferGenerations) {
                    if (retained != nullptr) {
                        stats.retainedProvidedBufferBytes +=
                            retained->m_bufferSize * retained->m_bufferCount;
                    }
                }
                const auto* generation = m_impl->m_currentBufferGeneration.get(); // 只汇报新 read 目标代
                if (generation != nullptr && generation->m_bufferPool != nullptr) {
                    stats.receiveBundleEnabled = generation->m_receiveBundleEnabled;
                    stats.providedBufferCount = generation->m_bufferPool->BufferCount();
                    stats.providedBufferSize = generation->m_bufferPool->BufferSize();
                    stats.activeBufferLeases = generation->m_bufferPool->ActiveLeaseCount();
                    stats.pendingBufferReturns = generation->m_bufferPool->PendingReturnCount();
                    stats.currentAvailableBuffers = generation->m_bufferPool->CurrentAvailableCount();
                    stats.minimumAvailableBuffers = generation->m_bufferPool->MinimumAvailableCount();
                    stats.leaseHoldSampleCount = generation->m_bufferPool->LeaseHoldSampleCount();
                    stats.totalLeaseHoldNanoseconds = generation->m_bufferPool->TotalLeaseHoldNanoseconds();
                    stats.maximumLeaseHoldNanoseconds = generation->m_bufferPool->MaximumLeaseHoldNanoseconds();
                }
                return stats;
            }

            bool IoUringPoller::ReconfigureProvidedBuffers(
                std::size_t bufferSize,
                std::uint32_t bufferCount) noexcept {
                if (m_impl == nullptr || !m_impl->m_initialized || !m_impl->m_activated) {
                    SetLastError(EINVAL);
                    return false;
                }

                int generationError = 0; // 注册失败时保持当前 generation 完整不变
                try {
                    std::unique_lock<std::mutex> lock(m_impl->m_bufferGenerationMutex); // 串行新旧指针和统计
                    if (m_impl->m_currentBufferGeneration
                        && m_impl->m_currentBufferGeneration->m_bufferSize == bufferSize
                        && m_impl->m_currentBufferGeneration->m_bufferCount == bufferCount) {
                        return true;
                    }
                    if (!m_impl->ValidateBufferDimensions(bufferSize, bufferCount)) {
                        SetLastError(EINVAL);
                        return false;
                    }

                    // 先保证旧 generation 入队不会分配，再注册新 group，形成强回滚边界。
                    m_impl->m_retiringBufferGenerations.reserve(
                        m_impl->m_retiringBufferGenerations.size() + 1);
                    m_impl->m_retainedBufferGenerations.reserve(
                        m_impl->m_retainedBufferGenerations.size()
                            + m_impl->m_retiringBufferGenerations.size() + 1);
                    auto nextGeneration = m_impl->CreateBufferGeneration(
                        bufferSize,
                        bufferCount,
                        generationError);
                    if (!nextGeneration) {
                        SetLastError(generationError != 0 ? generationError : ENOMEM);
                        return false;
                    }

                    if (m_impl->m_currentBufferGeneration) {
                        m_impl->m_currentBufferGeneration->m_retiring = true;
                        m_impl->m_retiringBufferGenerations.push_back(
                            std::move(m_impl->m_currentBufferGeneration));
                    }
                    m_impl->m_currentBufferGeneration = std::move(nextGeneration); // 后续 read 只绑定新 group
                }
                catch (...) {
                    SetLastError(ENOMEM);
                    return false;
                }

                m_impl->m_bufferReconfigurationCount.fetch_add(1, std::memory_order_relaxed);
                m_impl->m_completionBudget.store(
                    std::clamp<unsigned int>(bufferCount, 64U, m_impl->m_ringEntries),
                    std::memory_order_relaxed);
                m_impl->RetryPendingOperationCancellations(); // 旧代 read 与其他待取消 operation 统一重试
                for (auto& item : m_impl->m_connectionStates) {
                    IoUringPollerImpl::ConnectionState& state = *item.second;
                    if (!state.m_closing && state.m_readEnabled && !state.m_readOutstanding
                        && PollerAccess::ReadEnabled(*state.m_connection)) {
                        (void)m_impl->SubmitRead(state);
                    }
                }
                m_impl->RetireUnusedBufferGenerations();
                return true;
            }

            const char* IoUringPoller::BackendName() const noexcept {
                return "io_uring-multishot-provided-buffer";
            }
        }
    }
}

#endif
