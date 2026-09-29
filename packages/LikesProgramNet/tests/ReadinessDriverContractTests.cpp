#include "net/platform/ReadinessDriver.hpp"
#include "net/platform/SocketOps.hpp"
#include "net/platform/posix/ReadinessCompletionPoller.hpp"

#include <LikesProgram/Net/Address.hpp>
#include <LikesProgram/Net/Buffer.hpp>
#include <LikesProgram/Net/Connection.hpp>
#include <stdexcept>
#include <memory>
#include <chrono>
#include <netinet/in.h>
#include <sys/socket.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {
    using LikesProgram::Net::IOEvent;
    using LikesProgram::Net::Poller;
    using LikesProgram::Net::SocketLength;
    using LikesProgram::Net::SocketType;
    using LikesProgram::Net::Internal::ReadinessDriver;
    using LikesProgram::Net::Internal::ReadinessEvent;
    using LikesProgram::Net::Internal::ReadinessRegistrationId;

    // 在失败时保留 fake driver 的契约名称。
    void Require(bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    }

    // 只记录 registration/wait 语义，不接触生产 Poller 状态。
    class FakeReadinessDriver final : public ReadinessDriver {
    public:
        bool Activate() override {
            m_active = true;
            return true;
        }

        bool Add(ReadinessRegistrationId id, SocketType fd, IOEvent events) override {
            m_registrations[id] = { fd, events };
            return true;
        }

        bool Modify(ReadinessRegistrationId id, SocketType fd, IOEvent events) override {
            const auto found = m_registrations.find(id);
            if (found == m_registrations.end() || found->second.first != fd) return false;
            found->second.second = events;
            return true;
        }

        bool Remove(ReadinessRegistrationId id, SocketType) noexcept override {
            return m_registrations.erase(id) != 0;
        }

        int Wait(int timeoutMs, std::vector<ReadinessEvent>& events) override {
            m_lastTimeoutMs = timeoutMs; // 记录 common Poller 计算出的 deadline 预算
            events = std::move(m_ready);
            m_ready.clear();
            return static_cast<int>(events.size());
        }

        int LastError() const noexcept override {
            return 0;
        }

        const char* BackendName() const noexcept override {
            return "fake-readiness";
        }

        void Push(ReadinessEvent event) {
            m_ready.push_back(event);
        }

        // 返回当前唯一 registration，供身份稳定性断言使用。
        ReadinessRegistrationId OnlyRegistrationId() const {
            Require(m_registrations.size() == 1,
                "Fake readiness driver should contain one registration");
            return m_registrations.begin()->first;
        }

        // 返回当前唯一 registration 的关注事件。
        IOEvent OnlyEvents() const {
            Require(m_registrations.size() == 1,
                "Fake readiness driver should contain one interest set");
            return m_registrations.begin()->second.second;
        }

        bool m_active = false; // fake driver 是否完成激活
        std::unordered_map<ReadinessRegistrationId, std::pair<SocketType, IOEvent>> m_registrations; // 当前 registration
        std::vector<ReadinessEvent> m_ready; // 下一轮 Wait 返回的事件
        int m_lastTimeoutMs = -1; // 最近一轮 driver wait 的毫秒预算
    };

    // 验证 fake driver 能表达稳定 registration 与逻辑事件。
    void TestReadinessDriverContract() {
        FakeReadinessDriver driver;
        Require(driver.Activate(), "Fake readiness driver should activate");
        Require(driver.Add(1, 7, IOEvent::Read), "Fake readiness driver should add");
        Require(driver.Modify(1, 7, IOEvent::Read | IOEvent::Write),
            "Fake readiness driver should modify");
        driver.Push({ 1, IOEvent::Read, 0 });

        std::vector<ReadinessEvent> events; // 当前轮逻辑事件快照
        Require(driver.Wait(0, events) == 1, "Fake readiness driver should return one event");
        Require(events.front().registrationId == 1,
            "Readiness event should preserve registration identity");
        Require(driver.Remove(1, 7), "Fake readiness driver should remove");
    }

    // 验证 common Poller 保持 Channel registration 身份并过滤迟到事件。
    void TestCommonPollerChannelRegistrationIdentity() {
        auto driver = std::make_unique<FakeReadinessDriver>();
        FakeReadinessDriver* driverObserver = driver.get(); // 观察 fake driver 的 registration
        LikesProgram::Net::Internal::ReadinessCompletionPoller poller(
            nullptr,
            std::move(driver));
        Require(poller.Activate(), "Common Poller should activate its driver");

        const SocketType channelFd = static_cast<SocketType>(42); // 不接触内核的稳定测试 fd
        LikesProgram::Net::Channel channel(nullptr, channelFd, IOEvent::Read); // 待迁移的 Channel
        Require(poller.AddChannel(&channel), "Common Poller should add Channel");
        const ReadinessRegistrationId firstId = driverObserver->OnlyRegistrationId();
        Require(poller.UpdateChannel(&channel), "Common Poller should update Channel");
        Require(driverObserver->OnlyRegistrationId() == firstId,
            "Channel update should preserve registration identity");
        Require(poller.RemoveChannel(&channel), "Common Poller should remove Channel");

        driverObserver->Push({ firstId, IOEvent::Read, 0 }); // 删除后的迟到事件必须被忽略
        std::vector<LikesProgram::Net::Channel*> activeChannels; // 当前轮待分发 Channel 快照
        poller.Poll(0, activeChannels);
        Require(activeChannels.empty(), "Removed registration event should be ignored");
    }

    // 验证 common Poller 的 timer generation、取消和等待预算语义。
    void TestCommonPollerTimeoutGeneration() {
        auto driver = std::make_unique<FakeReadinessDriver>();
        FakeReadinessDriver* driverObserver = driver.get(); // 观察 fake driver 的 wait 预算
        LikesProgram::Net::Internal::ReadinessCompletionPoller poller(
            nullptr,
            std::move(driver));
        Require(poller.Activate(), "Common Poller should activate its timer driver");

        int fired = 0; // active timer 的触发次数
        int canceledFired = 0; // canceled timer 不应触发
        const Poller::TimeoutId activeId = poller.ScheduleTimeout(
            std::chrono::milliseconds(0),
            [&fired]() { ++fired; });
        const Poller::TimeoutId canceledId = poller.ScheduleTimeout(
            std::chrono::milliseconds(10),
            [&canceledFired]() { ++canceledFired; });
        Require(activeId != Poller::InvalidTimeoutId,
            "Active timeout should return a valid id");
        Require(canceledId != Poller::InvalidTimeoutId,
            "Canceled timeout should return a valid id");
        poller.CancelTimeout(canceledId);

        std::vector<LikesProgram::Net::Channel*> activeChannels; // timer Poll 不产生 Channel
        poller.Poll(20, activeChannels);
        Require(fired == 1, "Active timeout should fire exactly once");
        Require(canceledFired == 0, "Canceled timeout must not run");
        Require(driverObserver->m_lastTimeoutMs <= 20,
            "Nearest timeout should bound driver wait");

        poller.Poll(0, activeChannels);
        Require(fired == 1, "Consumed timeout must not fire twice");
    }

    // 创建非阻塞 loopback listener，供 connect/accept completion 使用。
    SocketType CreateLoopbackListener(LikesProgram::Net::Address& address) {
        SocketType listener = LikesProgram::Net::Internal::CreateSocket(
            AF_INET,
            SOCK_STREAM,
            IPPROTO_TCP);
        Require(listener != LikesProgram::Net::kInvalidSocket,
            "Loopback listener socket should be created");
        Require(LikesProgram::Net::Internal::SetNonBlocking(listener),
            "Loopback listener should be nonblocking");

        sockaddr_in localAddress{}; // 绑定到本机回环的临时端口
        localAddress.sin_family = AF_INET;
        localAddress.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        localAddress.sin_port = htons(0);
        Require(LikesProgram::Net::Internal::BindSocket(
            listener,
            reinterpret_cast<const sockaddr*>(&localAddress),
            static_cast<SocketLength>(sizeof(localAddress))) == 0,
            "BindSocket should bind loopback listener");
        Require(LikesProgram::Net::Internal::ListenSocket(listener, 8) == 0,
            "ListenSocket should start loopback listener");
        address = LikesProgram::Net::Address::GetLocalAddress(listener);
        Require(address.IsValid(), "Loopback listener should expose a valid address");
        return listener;
    }

    // 验证 connect completion 的立即结果、readiness 结果和取消边界。
    void TestCommonPollerConnectCompletion() {
        auto driver = std::make_unique<FakeReadinessDriver>();
        FakeReadinessDriver* driverObserver = driver.get(); // 观察 connect registration
        LikesProgram::Net::Internal::ReadinessCompletionPoller poller(
            nullptr,
            std::move(driver));
        Require(poller.Activate(), "Common Poller should activate connect driver");

        LikesProgram::Net::Address listenerAddress; // connect 目标地址快照
        const SocketType listener = CreateLoopbackListener(listenerAddress);
        int connectCallbacks = 0; // connect callback 次数
        int connectError = -1; // callback 收到的 SO_ERROR
        const Poller::ConnectId connectId = poller.StartConnect(
            listenerAddress,
            [&connectCallbacks, &connectError](SocketType fd, int error) {
                ++connectCallbacks;
                connectError = error;
                if (fd != LikesProgram::Net::kInvalidSocket) {
                    LikesProgram::Net::Internal::CloseSocket(fd);
                }
            });
        Require(connectId != Poller::InvalidConnectId,
            "Common Poller should accept connect");
        if (!driverObserver->m_registrations.empty()) {
            driverObserver->Push({
                driverObserver->OnlyRegistrationId(),
                IOEvent::Write,
                0 }); // EINPROGRESS readiness 只投递一次 completion
        }
        std::vector<LikesProgram::Net::Channel*> activeChannels; // connect 不产生 Channel
        poller.Poll(0, activeChannels);
        Require(connectCallbacks == 1, "Connect completion should run exactly once");
        Require(connectError == 0, "Loopback connect should complete without error");
        LikesProgram::Net::Internal::CloseSocket(listener);
    }

    // 验证取消 connect 后的迟到 readiness 不会调用用户 callback。
    void TestCommonPollerConnectCancellation() {
        auto driver = std::make_unique<FakeReadinessDriver>();
        FakeReadinessDriver* driverObserver = driver.get(); // 观察取消后的 registration
        LikesProgram::Net::Internal::ReadinessCompletionPoller poller(
            nullptr,
            std::move(driver));
        Require(poller.Activate(), "Common Poller should activate cancel driver");

        LikesProgram::Net::Address listenerAddress; // connect 目标地址快照
        const SocketType listener = CreateLoopbackListener(listenerAddress);
        int canceledCallbacks = 0; // cancel 后必须保持零
        const Poller::ConnectId connectId = poller.StartConnect(
            listenerAddress,
            [&canceledCallbacks](SocketType fd, int) {
                ++canceledCallbacks;
                if (fd != LikesProgram::Net::kInvalidSocket) {
                    LikesProgram::Net::Internal::CloseSocket(fd);
                }
            });
        Require(connectId != Poller::InvalidConnectId,
            "Common Poller should accept cancelable connect");
        if (!driverObserver->m_registrations.empty()) {
            driverObserver->Push({
                driverObserver->OnlyRegistrationId(),
                IOEvent::Write,
                0 }); // 保存一个会在 Cancel 后到达的 stale event
        }
        poller.CancelConnect(connectId);
        std::vector<LikesProgram::Net::Channel*> activeChannels; // cancel 不产生 Channel
        poller.Poll(0, activeChannels);
        Require(canceledCallbacks == 0, "Canceled connect must suppress callback");
        LikesProgram::Net::Internal::CloseSocket(listener);
    }

    // 验证一轮 listener readiness 会 drain 全部已排队连接。
    void TestCommonPollerAcceptDrain() {
        auto driver = std::make_unique<FakeReadinessDriver>();
        FakeReadinessDriver* driverObserver = driver.get(); // 观察 accept registration
        LikesProgram::Net::Internal::ReadinessCompletionPoller poller(
            nullptr,
            std::move(driver));
        Require(poller.Activate(), "Common Poller should activate accept driver");

        LikesProgram::Net::Address listenerAddress; // 三个 client 共用的目标地址
        const SocketType listener = CreateLoopbackListener(listenerAddress);
        SocketType clients[3] = { // 保持 client 所有权直到 accept queue 建立
            LikesProgram::Net::Internal::CreateSocket(AF_INET, SOCK_STREAM, IPPROTO_TCP),
            LikesProgram::Net::Internal::CreateSocket(AF_INET, SOCK_STREAM, IPPROTO_TCP),
            LikesProgram::Net::Internal::CreateSocket(AF_INET, SOCK_STREAM, IPPROTO_TCP) };
        for (SocketType client : clients) {
            Require(client != LikesProgram::Net::kInvalidSocket,
                "Accept client socket should be created");
            Require(LikesProgram::Net::Internal::ConnectSocket(
                client,
                listenerAddress.SockAddr(),
                listenerAddress.Length()) == 0,
                "Accept client should connect loopback listener");
        }

        int acceptedCount = 0; // 当前 readiness 窗口 drain 的连接数
        Require(poller.StartAccept(listener, [&acceptedCount](SocketType fd) {
            ++acceptedCount;
            LikesProgram::Net::Internal::CloseSocket(fd);
        }), "Common Poller should start accept");
        const ReadinessRegistrationId registrationId = driverObserver->OnlyRegistrationId();
        driverObserver->Push({ registrationId, IOEvent::Read, 0 }); // 一轮 readiness 触发 drain
        std::vector<LikesProgram::Net::Channel*> activeChannels; // accept 不产生 Channel
        poller.Poll(0, activeChannels);
        Require(acceptedCount == 3, "One readiness window should drain three accepts");

        for (SocketType client : clients) {
            LikesProgram::Net::Internal::CloseSocket(client);
        }
        LikesProgram::Net::Internal::CloseSocket(listener);
    }

    // 验证停止 listener 后的 stale readiness 不再接受连接。
    void TestCommonPollerAcceptCancellation() {
        auto driver = std::make_unique<FakeReadinessDriver>();
        FakeReadinessDriver* driverObserver = driver.get(); // 观察停止后的 registration
        LikesProgram::Net::Internal::ReadinessCompletionPoller poller(
            nullptr,
            std::move(driver));
        Require(poller.Activate(), "Common Poller should activate accept cancel driver");

        LikesProgram::Net::Address listenerAddress; // 仅用于建立 listener
        const SocketType listener = CreateLoopbackListener(listenerAddress);
        int lateAcceptCount = 0; // StopAccept 后必须保持零
        Require(poller.StartAccept(listener, [&lateAcceptCount](SocketType fd) {
            ++lateAcceptCount;
            LikesProgram::Net::Internal::CloseSocket(fd);
        }), "Common Poller should start cancelable accept");
        const ReadinessRegistrationId registrationId = driverObserver->OnlyRegistrationId();
        poller.StopAccept(listener);
        driverObserver->Push({ registrationId, IOEvent::Read, 0 }); // 已撤销 registration 的迟到事件
        std::vector<LikesProgram::Net::Channel*> activeChannels; // stale accept 不产生 Channel
        poller.Poll(0, activeChannels);
        Require(lateAcceptCount == 0, "Stopped listener must ignore stale readiness");
        LikesProgram::Net::Internal::CloseSocket(listener);
    }

    class CapturingTcpConnection final : public LikesProgram::Net::Connection {
    public:
        explicit CapturingTcpConnection(SocketType fd)
            : Connection(fd, nullptr) {
        }

        std::string m_payload; // 已消费的 TCP payload
        int m_writeCompleteCount = 0; // 写链排空通知次数

    protected:
        // 消费 common Poller 交付的完整 owning Buffer。
        void OnMessage(LikesProgram::Net::Buffer& input) override {
            const std::size_t readableBytes = input.ReadableBytes(); // 当前 completion 字节数
            m_payload.append(
                reinterpret_cast<const char*>(input.Peek()),
                readableBytes);
            input.Consume(readableBytes);
        }

        // 记录 common Poller 写链排空通知。
        void OnWriteComplete() override {
            ++m_writeCompleteCount;
        }
    };

    // 验证 common Poller 的 TCP owning read/write、读开关与 peer EOF。
    void TestCommonPollerTcpOwnership() {
        auto driver = std::make_unique<FakeReadinessDriver>();
        FakeReadinessDriver* driverObserver = driver.get(); // 观察 TCP interest 变化
        LikesProgram::Net::Internal::ReadinessCompletionPoller poller(
            nullptr,
            std::move(driver));
        Require(poller.Activate(), "Common Poller should activate TCP driver");

        int sockets[2] = { -1, -1 }; // socketpair 隔离 common TCP completion
        Require(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0,
            "Common TCP socketpair should open");
        Require(LikesProgram::Net::Internal::SetNonBlocking(sockets[0])
            && LikesProgram::Net::Internal::SetNonBlocking(sockets[1]),
            "Common TCP sockets should be nonblocking");

        auto connection = std::make_shared<CapturingTcpConnection>(sockets[0]);
        Require(poller.StartConnection(connection),
            "Common Poller should start TCP connection");
        const ReadinessRegistrationId registrationId = driverObserver->OnlyRegistrationId();

        const char inbound[] = "common-read"; // peer 写入的明文 payload
        Require(LikesProgram::Net::Internal::SendSocket(
            sockets[1],
            inbound,
            sizeof(inbound) - 1,
            0) == static_cast<std::int64_t>(sizeof(inbound) - 1),
            "TCP peer should send common payload");
        driverObserver->Push({ registrationId, IOEvent::Read, 0 }); // 模拟 read readiness
        std::vector<LikesProgram::Net::Channel*> activeChannels; // 直接 TCP 不产生 Channel
        poller.Poll(0, activeChannels);
        Require(connection->m_payload == inbound,
            "Common readiness TCP should preserve payload order");

        LikesProgram::Net::Buffer outbound; // 移动进入 common BufferChain 的 payload
        outbound.Append("common-write", 12);
        Require(poller.QueueWrite(connection.get(), std::move(outbound)),
            "Common Poller should accept TCP Buffer ownership");
        driverObserver->Push({ registrationId, IOEvent::Write, 0 }); // 模拟 write readiness
        poller.Poll(0, activeChannels);
        char received[16]{}; // peer 读取的 common write payload
        const std::int64_t receivedBytes = LikesProgram::Net::Internal::ReceiveSocket(
            sockets[1],
            received,
            sizeof(received),
            0);
        Require(receivedBytes == 12
            && std::string_view(received, static_cast<std::size_t>(receivedBytes)) == "common-write",
            "Common readiness TCP should preserve write bytes");
        Require(connection->m_writeCompleteCount == 1,
            "Common readiness TCP should complete one BufferChain once");

        poller.SetReadEnabled(connection.get(), false);
        Require(!LikesProgram::Net::HasEvent(driverObserver->OnlyEvents(), IOEvent::Read),
            "Paused common TCP should remove read interest");
        poller.SetReadEnabled(connection.get(), true);
        Require(LikesProgram::Net::HasEvent(driverObserver->OnlyEvents(), IOEvent::Read),
            "Resumed common TCP should restore read interest");

        Require(::shutdown(sockets[1], SHUT_WR) == 0,
            "TCP peer should publish EOF");
        driverObserver->Push({ registrationId, IOEvent::Read | IOEvent::Close, 0 });
        poller.Poll(0, activeChannels);
        Require(connection->GetState() == LikesProgram::Net::Connection::State::Closed,
            "EOF should converge the common Connection state");
        poller.StopConnection(connection.get());
        LikesProgram::Net::Internal::CloseSocket(sockets[1]);
    }

    class CapturingUdpConnection final : public LikesProgram::Net::Connection {
    public:
        explicit CapturingUdpConnection(SocketType fd)
            : Connection(fd, nullptr, LikesProgram::Net::TransportKind::Udp) {
        }

        std::vector<std::string> m_payloads; // 按接收顺序保存数据报 payload
        std::vector<std::size_t> m_originalBytes; // 每个数据报的原始长度
        std::vector<bool> m_truncated; // 每个数据报的截断标记

    protected:
        // 消费 common Poller 交付的 UDP 数据报与边界元数据。
        void OnDatagram(
            LikesProgram::Net::Buffer& input,
            const LikesProgram::Net::Address&,
            std::size_t originalBytes,
            bool truncated) override {
            const std::size_t readableBytes = input.ReadableBytes(); // 当前可见 payload 长度
            m_payloads.emplace_back(
                reinterpret_cast<const char*>(input.Peek()),
                readableBytes);
            m_originalBytes.push_back(originalBytes);
            m_truncated.push_back(truncated);
            input.Consume(readableBytes);
        }
    };

    // 创建非阻塞 loopback UDP socket 并返回绑定地址。
    SocketType CreateBoundUdpSocket(LikesProgram::Net::Address& address) {
        const SocketType fd = LikesProgram::Net::Internal::CreateSocket(
            AF_INET,
            SOCK_DGRAM | SOCK_CLOEXEC,
            IPPROTO_UDP); // 当前测试持有的 UDP socket
        Require(fd != LikesProgram::Net::kInvalidSocket,
            "Common UDP socket should be created");
        Require(LikesProgram::Net::Internal::SetNonBlocking(fd),
            "Common UDP socket should be nonblocking");

        sockaddr_in localAddress{}; // 绑定到回环临时端口
        localAddress.sin_family = AF_INET;
        localAddress.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        localAddress.sin_port = htons(0);
        Require(LikesProgram::Net::Internal::BindSocket(
            fd,
            reinterpret_cast<const sockaddr*>(&localAddress),
            static_cast<SocketLength>(sizeof(localAddress))) == 0,
            "Common UDP socket should bind loopback");
        address = LikesProgram::Net::Address::GetLocalAddress(fd);
        Require(address.IsValid(), "Common UDP socket should expose a valid address");
        return fd;
    }

    // 验证 common Poller 的 UDP 边界、显式 peer、零长度与能力归零。
    void TestCommonPollerUdpOwnership() {
        auto driver = std::make_unique<FakeReadinessDriver>();
        FakeReadinessDriver* driverObserver = driver.get(); // 观察 UDP registration
        LikesProgram::Net::Internal::ReadinessCompletionPoller poller(
            nullptr,
            std::move(driver));
        Require(poller.Activate(), "Common Poller should activate UDP driver");

        LikesProgram::Net::Address serverAddress; // UDP Connection 的本地地址
        LikesProgram::Net::Address peerAddress; // 显式回包目标地址
        const SocketType serverFd = CreateBoundUdpSocket(serverAddress);
        const SocketType peerFd = CreateBoundUdpSocket(peerAddress);
        auto connection = std::make_shared<CapturingUdpConnection>(serverFd);
        connection->SetMaxDatagramBytes(4);
        Require(poller.StartConnection(connection),
            "Common Poller should start UDP connection");
        const ReadinessRegistrationId registrationId = driverObserver->OnlyRegistrationId();

        const char oversized[] = "abcdef"; // 超过四字节容量的输入数据报
        Require(LikesProgram::Net::Internal::SendSocketTo(
            peerFd,
            oversized,
            sizeof(oversized) - 1,
            0,
            serverAddress.SockAddr(),
            serverAddress.Length()) == static_cast<std::int64_t>(sizeof(oversized) - 1),
            "UDP peer should send oversized datagram");
        driverObserver->Push({ registrationId, IOEvent::Read, 0 }); // 模拟 UDP read readiness
        std::vector<LikesProgram::Net::Channel*> activeChannels; // UDP 不产生 Channel
        poller.Poll(0, activeChannels);
        Require(connection->m_payloads.size() == 1
            && connection->m_payloads.front() == "abcd",
            "Common UDP should preserve truncated payload bytes");
        Require(connection->m_originalBytes.front() == 6
            && connection->m_truncated.front(),
            "Common UDP should preserve original length and truncation");

        LikesProgram::Net::Buffer response; // 显式 peer 的完整回包
        response.Append("pong", 4);
        Require(poller.QueueDatagramWrite(connection.get(), peerAddress, std::move(response)),
            "Common Poller should queue explicit-peer datagram");
        Require(poller.PendingWriteBytes(connection.get()) == 4,
            "Common UDP pending cost should preserve payload bytes");
        poller.Poll(0, activeChannels); // Flush 先尝试发送完整数据报
        char received[8]{}; // peer 读取的完整回包
        sockaddr_storage sourceStorage{}; // 回包来源地址
        SocketLength sourceLength = static_cast<SocketLength>(sizeof(sourceStorage));
        const std::int64_t receivedBytes = LikesProgram::Net::Internal::ReceiveSocketFrom(
            peerFd,
            received,
            sizeof(received),
            0,
            reinterpret_cast<sockaddr*>(&sourceStorage),
            &sourceLength);
        Require(receivedBytes == 4
            && std::string_view(received, 4) == "pong",
            "Common UDP should preserve explicit-peer response bytes");

        LikesProgram::Net::Buffer empty(0); // 零长度数据报仍占一个 queue cost
        Require(poller.QueueDatagramWrite(connection.get(), peerAddress, std::move(empty)),
            "Common Poller should queue zero-length datagram");
        Require(poller.PendingWriteBytes(connection.get()) == 1,
            "Zero-length datagram should retain one queue-cost unit");
        poller.Poll(0, activeChannels);
        sourceLength = static_cast<SocketLength>(sizeof(sourceStorage));
        Require(LikesProgram::Net::Internal::ReceiveSocketFrom(
            peerFd,
            received,
            sizeof(received),
            0,
            reinterpret_cast<sockaddr*>(&sourceStorage),
            &sourceLength) == 0,
            "Common UDP should send one zero-length datagram");

        const LikesProgram::Net::CompletionStats stats = poller.GetCompletionStats();
        Require(!stats.datagramMultishotEnabled
            && stats.datagramProvidedBufferCount == 0
            && stats.datagramActiveBufferLeases == 0,
            "Readiness core must not emulate io_uring UDP capabilities");
        Require(stats.maximumDatagramSendBatch <= 1,
            "Readiness core should report one datagram per send syscall");
        poller.StopConnection(connection.get());
        LikesProgram::Net::Internal::CloseSocket(peerFd);
    }
}

// 执行私有 readiness driver 契约。
int main() {
    TestReadinessDriverContract();
    TestCommonPollerChannelRegistrationIdentity();
    TestCommonPollerTimeoutGeneration();
    TestCommonPollerConnectCompletion();
    TestCommonPollerConnectCancellation();
    TestCommonPollerAcceptDrain();
    TestCommonPollerAcceptCancellation();
    TestCommonPollerTcpOwnership();
    TestCommonPollerUdpOwnership();
    return 0;
}
