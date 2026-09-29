#include "net/platform/windows/IocpPoller.hpp"
#include "net/platform/SocketOps.hpp"
#include <LikesProgram/Net/Address.hpp>
#include <LikesProgram/Net/Connection.hpp>
#include <LikesProgram/Net/Client.hpp>
#include <LikesProgram/Net/EventLoop.hpp>
#include <LikesProgram/Net/Server.hpp>
#include <LikesProgram/Net/TlsEngine.hpp>
#include <LikesProgram/Net/TlsEngineFactory.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <set>
#include <stdexcept>
#include <thread>
#include <atomic>
#include <vector>

namespace {

void Require(bool condition, const char* message) {
    if (condition) return;
    std::cerr << message << std::endl;
    std::exit(1);
}

using LikesProgram::Net::Channel;
using LikesProgram::Net::CompletionStats;
using LikesProgram::Net::EventLoop;
using LikesProgram::Net::IOEvent;
using LikesProgram::Net::SocketLength;
using LikesProgram::Net::SocketType;
using LikesProgram::Net::Server;
using LikesProgram::Net::Internal::IocpPoller;
namespace Internal = LikesProgram::Net::Internal;

struct DatagramPair final {
    SocketType receiver = LikesProgram::Net::kInvalidSocket;
    SocketType sender = LikesProgram::Net::kInvalidSocket;
    sockaddr_in address{};

    DatagramPair() = default;

    ~DatagramPair() {
        Internal::CloseSocket(receiver);
        Internal::CloseSocket(sender);
    }

    DatagramPair(const DatagramPair&) = delete;
    DatagramPair& operator=(const DatagramPair&) = delete;

    DatagramPair(DatagramPair&& other) noexcept
        : receiver(other.receiver), sender(other.sender), address(other.address) {
        other.receiver = LikesProgram::Net::kInvalidSocket;
        other.sender = LikesProgram::Net::kInvalidSocket;
    }

    DatagramPair& operator=(DatagramPair&& other) noexcept {
        if (this == &other) return *this;
        Internal::CloseSocket(receiver);
        Internal::CloseSocket(sender);
        receiver = other.receiver;
        sender = other.sender;
        address = other.address;
        other.receiver = LikesProgram::Net::kInvalidSocket;
        other.sender = LikesProgram::Net::kInvalidSocket;
        return *this;
    }
};

struct TcpListener final {
    SocketType fd = LikesProgram::Net::kInvalidSocket;
    LikesProgram::Net::Address address;

    ~TcpListener() {
        Internal::CloseSocket(fd);
    }

    TcpListener() = default;
    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    TcpListener(TcpListener&& other) noexcept
        : fd(other.fd), address(std::move(other.address)) {
        other.fd = LikesProgram::Net::kInvalidSocket;
    }

    TcpListener& operator=(TcpListener&& other) noexcept {
        if (this == &other) return *this;
        Internal::CloseSocket(fd);
        fd = other.fd;
        address = std::move(other.address);
        other.fd = LikesProgram::Net::kInvalidSocket;
        return *this;
    }
};

TcpListener MakeTcpListener() {
    TcpListener listener;
    listener.fd = Internal::CreateSocket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    Require(listener.fd != LikesProgram::Net::kInvalidSocket,
        "IOCP ConnectEx listener should open");
    Require(Internal::SetReuseAddress(listener.fd),
        "IOCP ConnectEx listener should enable address reuse");

    sockaddr_in requested{};
    requested.sin_family = AF_INET;
    requested.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    requested.sin_port = 0;
    Require(Internal::BindSocket(
        listener.fd,
        reinterpret_cast<const sockaddr*>(&requested),
        static_cast<SocketLength>(sizeof(requested))) == 0,
        "IOCP ConnectEx listener should bind");
    Require(Internal::ListenSocket(listener.fd, 8) == 0,
        "IOCP ConnectEx listener should listen");

    sockaddr_storage bound{};
    SocketLength boundLength = static_cast<SocketLength>(sizeof(bound));
    Require(::getsockname(
        listener.fd,
        reinterpret_cast<sockaddr*>(&bound),
        &boundLength) == 0,
        "IOCP ConnectEx listener should expose address");
    listener.address = LikesProgram::Net::Address(bound, boundLength);
    return listener;
}

SocketType ConnectTcpClient(const LikesProgram::Net::Address& address) {
    const SocketType client = Internal::CreateSocket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    Require(client != LikesProgram::Net::kInvalidSocket,
        "IOCP AcceptEx client should open");
    Require(Internal::ConnectSocket(
        client,
        address.SockAddr(),
        address.Length()) == 0,
        "IOCP AcceptEx client should connect");
    return client;
}

DatagramPair MakeDatagramPair() {
    DatagramPair pair;
    pair.receiver = Internal::CreateSocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    pair.sender = Internal::CreateSocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    Require(pair.receiver != LikesProgram::Net::kInvalidSocket
        && pair.sender != LikesProgram::Net::kInvalidSocket,
        "IOCP Channel test sockets should open");

    pair.address.sin_family = AF_INET;
    pair.address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    pair.address.sin_port = 0;
    Require(Internal::BindSocket(
        pair.receiver,
        reinterpret_cast<const sockaddr*>(&pair.address),
        static_cast<SocketLength>(sizeof(pair.address))) == 0,
        "IOCP Channel receiver should bind");
    SocketLength addressLength = static_cast<SocketLength>(sizeof(pair.address));
    Require(::getsockname(
        pair.receiver,
        reinterpret_cast<sockaddr*>(&pair.address),
        &addressLength) == 0,
        "IOCP Channel receiver should expose its address");
    sockaddr_in senderAddress{}; // sender 绑定 loopback，保证反向 SendTo 目标可路由
    senderAddress.sin_family = AF_INET;
    senderAddress.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    senderAddress.sin_port = 0;
    Require(Internal::BindSocket(
        pair.sender,
        reinterpret_cast<const sockaddr*>(&senderAddress),
        static_cast<SocketLength>(sizeof(senderAddress))) == 0,
        "IOCP Channel sender should bind a loopback source");
    Require(Internal::SetNonBlocking(pair.receiver)
        && Internal::SetNonBlocking(pair.sender),
        "IOCP Channel sockets should be nonblocking");
    return pair;
}

bool HasNetworkEvent(IOEvent events, IOEvent expected) {
    return LikesProgram::Net::HasEvent(events, expected);
}

bool WaitUntil(const std::function<bool()>& predicate, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout; // 当前断言的等待上界
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

struct IocpTcpProbe final {
    std::atomic<bool> connected{ false }; // OnConnected 已在 issuer 线程执行
    std::atomic<bool> messageReceived{ false }; // owning read 已交付业务 payload
    std::atomic<bool> writeCompleted{ false }; // owning write 链已经排空
    std::atomic<bool> closed{ false }; // peer EOF 已进入统一关闭路径
    std::atomic<int> error{ 0 }; // 最近一次连接级错误
    bool pauseOnConnect = false; // 启动回调内先暂停 read，验证 CancelIoEx/重提边界
    bool largeWrite = false; // 启动回调内发送大 payload，验证 partial write 链
    std::string expectedOutbound; // 大 payload 的稳定测试快照
    std::mutex payloadMutex; // 保护业务 payload 快照
    std::string payload; // OnMessage 消费前的完整输入
};

class IocpPlainConnection final : public LikesProgram::Net::Connection {
public:
    IocpPlainConnection(SocketType fd, EventLoop* loop, IocpTcpProbe& probe)
        : Connection(fd, loop), m_probe(probe) {
    }

protected:
    // 启动回调内直接进入 Poller owning write 快路径。
    void OnConnected() override {
        m_probe.connected.store(true, std::memory_order_release);
        if (m_probe.pauseOnConnect) PauseReading();
        if (m_probe.largeWrite) {
            Send(m_probe.expectedOutbound.data(), m_probe.expectedOutbound.size());
        }
        else {
            Send("outbound", 8);
        }
    }

    // owning read 必须把完整 payload 交给业务并允许同步消费。
    void OnMessage(LikesProgram::Net::Buffer& input) override {
        {
            std::lock_guard<std::mutex> lock(m_probe.payloadMutex); // 发布输入快照
            m_probe.payload.assign(input.AsStringView());
        }
        input.RetrieveAll();
        m_probe.messageReceived.store(true, std::memory_order_release);
    }

    // 最后一个 WSASend completion 只通知一次写链排空。
    void OnWriteComplete() override {
        m_probe.writeCompleted.store(true, std::memory_order_release);
    }

    // peer EOF 关闭后发布测试终态。
    void OnClosed() override {
        m_probe.closed.store(true, std::memory_order_release);
    }

    // 保留真实 Winsock 错误，便于区分 EOF 与异常关闭。
    void OnError(int error) override {
        m_probe.error.store(error, std::memory_order_release);
    }

private:
    IocpTcpProbe& m_probe; // 测试线程与 issuer 线程共享的观察状态
};

struct IocpTlsProbe final {
    std::atomic<bool> connected{ false }; // OnConnected 已执行
    std::atomic<bool> handshakeDone{ false }; // Engine 已进入 Active
    std::atomic<bool> writeCompleted{ false }; // 应用密文或 close_notify 已排空
    std::atomic<bool> closed{ false }; // close_notify 写完后的统一关闭
    std::atomic<int> error{ 0 }; // 最近一次 TLS/连接错误
};

class IocpMemoryTlsEngine final : public LikesProgram::Net::TlsEngine {
public:
    explicit IocpMemoryTlsEngine(IocpTlsProbe& probe)
        : m_probe(probe) {
    }

    LikesProgram::Net::TlsResult StartHandshake(
        LikesProgram::Net::BufferChain&) override {
        m_state = LikesProgram::Net::TlsState::Active;
        return {};
    }

    LikesProgram::Net::TlsResult ConsumeCiphertext(
        LikesProgram::Net::BufferChain& input,
        LikesProgram::Net::BufferChain&,
        LikesProgram::Net::BufferChain&) override {
        input.Consume(input.ReadableBytes());
        return {};
    }

    LikesProgram::Net::TlsResult ConsumePlaintext(
        LikesProgram::Net::BufferChain& input,
        LikesProgram::Net::BufferChain& output) override {
        while (!input.Empty()) {
            const auto segment = input.Segment(0); // 每个输入段都转为一条测试密文
            LikesProgram::Net::Buffer encoded(0);
            encoded.Append("tls:", 4);
            encoded.Append(segment.Data(), segment.Size());
            output.Append(std::move(encoded));
            input.Consume(segment.Size());
        }
        return { LikesProgram::Net::TlsAction::CiphertextReady, 0 };
    }

    LikesProgram::Net::TlsResult Shutdown(
        LikesProgram::Net::BufferChain& output) override {
        LikesProgram::Net::Buffer closeNotify(0); // 模拟唯一 close_notify record
        closeNotify.Append("close", 5);
        output.Append(std::move(closeNotify));
        m_state = LikesProgram::Net::TlsState::Closed;
        return {
            LikesProgram::Net::TlsAction::CiphertextReady
                | LikesProgram::Net::TlsAction::CloseTransport,
            0
        };
    }

    LikesProgram::Net::TlsState State() const noexcept override {
        return m_state;
    }

    const char* NegotiatedProtocol() const noexcept override {
        return "iocp-test";
    }

private:
    IocpTlsProbe& m_probe; // 保持 Engine 与测试观察状态关联
    LikesProgram::Net::TlsState m_state = LikesProgram::Net::TlsState::Handshaking; // Engine 状态
};

class IocpTlsConnection final : public LikesProgram::Net::Connection {
public:
    IocpTlsConnection(SocketType fd, EventLoop* loop, IocpTlsProbe& probe)
        : Connection(fd, loop), m_probe(probe) {
        SetTlsEngineFactory(LikesProgram::Net::TlsEngineFactory([this]() {
            return std::make_unique<IocpMemoryTlsEngine>(m_probe);
        }));
    }

protected:
    // 连接建立后立即切入内存 TLS Engine，验证 completion 写链无需 socket 适配器。
    void OnConnected() override {
        m_probe.connected.store(true, std::memory_order_release);
        UpgradeCommunication();
        Send("app", 3);
    }

    // Active Engine 首次发布握手完成观察。
    void OnHandshakeDone() override {
        m_probe.handshakeDone.store(true, std::memory_order_release);
    }

    // close_notify 写链完成后发布统一关闭状态。
    void OnWriteComplete() override {
        m_probe.writeCompleted.store(true, std::memory_order_release);
    }

    // 保留真实错误码，便于区分 TLS 失败和正常 close_notify。
    void OnError(int error) override {
        m_probe.error.store(error, std::memory_order_release);
    }

    void OnClosed() override {
        m_probe.closed.store(true, std::memory_order_release);
    }

private:
    IocpTlsProbe& m_probe; // 测试线程与 issuer 线程共享状态
};

struct IocpUdpProbe final {
    std::atomic<bool> connected{ false }; // UDP Connection 已启动
    std::atomic<bool> writeCompleted{ false }; // UDP FIFO 已全部排空
    std::atomic<bool> closed{ false }; // ForceClose 后统一关闭
    std::atomic<int> error{ 0 }; // 最近一次 UDP 错误
    std::mutex mutex; // 保护多 peer 数据报快照
    std::vector<std::string> payloads; // 按接收顺序记录业务 payload
    std::vector<std::size_t> originalBytes; // 保存 wire datagram 原始长度
    std::vector<bool> truncated; // 保存每个数据报的截断标记
    std::vector<std::string> peers; // 保存每个数据报的 peer 文本
};

class IocpUdpConnection final : public LikesProgram::Net::Connection {
public:
    IocpUdpConnection(SocketType fd, EventLoop* loop, IocpUdpProbe& probe)
        : Connection(fd, loop, LikesProgram::Net::TransportKind::Udp), m_probe(probe) {
        SetMaxDatagramBytes(4); // wire buffer 保持完整，业务只接收前四字节
    }

protected:
    // UDP 直接 completion 启动后发布观察状态。
    void OnConnected() override {
        m_probe.connected.store(true, std::memory_order_release);
    }

    // 接收路径必须保留 payload、原始长度、截断与 peer 元数据。
    void OnDatagram(
        LikesProgram::Net::Buffer& input,
        const LikesProgram::Net::Address& peer,
        std::size_t originalBytes,
        bool truncated) override {
        std::lock_guard<std::mutex> lock(m_probe.mutex); // 保护多 peer 回调顺序
        m_probe.payloads.emplace_back(input.AsStringView());
        m_probe.originalBytes.push_back(originalBytes);
        m_probe.truncated.push_back(truncated);
        m_probe.peers.push_back(peer.ToString());
        input.RetrieveAll();
    }

    // FIFO 的最后一个 datagram completion 触发一次写完成。
    void OnWriteComplete() override {
        m_probe.writeCompleted.store(true, std::memory_order_release);
    }

    // 关闭时发布终态，便于测试等待 cancellation drain。
    void OnClosed() override {
        m_probe.closed.store(true, std::memory_order_release);
    }

    void OnError(int error) override {
        m_probe.error.store(error, std::memory_order_release);
    }

private:
    IocpUdpProbe& m_probe; // 测试线程与 issuer 线程共享观察状态
};

void TestIocpActivateCreatesCompletionPort() {
    IocpPoller poller(nullptr);
    Require(std::strcmp(poller.BackendName(), "iocp-overlapped") == 0,
        "IOCP backend name should be exact");
    Require(poller.Activate(), "IOCP Activate should create a completion port");
    Require(poller.Activate(), "IOCP Activate should be idempotent");
}

void TestIocpPollConsumesPostedPacket() {
    IocpPoller poller(nullptr);
    Require(poller.Activate(), "IOCP packet test should activate");
    poller.PostTestPacket();

    std::vector<Channel*> active;
    poller.Poll(100, active);
    Require(poller.CompletedOperationCount() == 1,
        "IOCP Poll should consume one posted completion packet");
}

void TestIocpTimeoutFiresExactlyOnce() {
    IocpPoller poller(nullptr);
    Require(poller.Activate(), "IOCP timeout test should activate");
    int fireCount = 0;
    const auto timeoutId = poller.ScheduleTimeout(
        std::chrono::milliseconds(20),
        [&fireCount]() { ++fireCount; });
    Require(timeoutId != IocpPoller::InvalidTimeoutId,
        "IOCP timeout should return a nonzero id");

    std::vector<Channel*> active;
    poller.Poll(200, active);
    poller.Poll(0, active);
    Require(fireCount == 1, "IOCP timeout should fire exactly once");
}

void TestIocpTimeoutCancelSuppressesCallback() {
    IocpPoller poller(nullptr);
    Require(poller.Activate(), "IOCP cancel test should activate");
    int fireCount = 0;
    const auto timeoutId = poller.ScheduleTimeout(
        std::chrono::milliseconds(20),
        [&fireCount]() { ++fireCount; });
    Require(timeoutId != IocpPoller::InvalidTimeoutId,
        "IOCP cancel test should return a nonzero id");
    poller.CancelTimeout(timeoutId);

    std::vector<Channel*> active;
    poller.Poll(80, active);
    Require(fireCount == 0, "Canceled IOCP timeout must not invoke callback");
}

void TestIocpTimeoutUsesNearestDeadline() {
    IocpPoller poller(nullptr);
    Require(poller.Activate(), "IOCP deadline test should activate");
    int shortCount = 0;
    int longCount = 0;
    const auto longId = poller.ScheduleTimeout(
        std::chrono::milliseconds(500),
        [&longCount]() { ++longCount; });
    const auto shortId = poller.ScheduleTimeout(
        std::chrono::milliseconds(20),
        [&shortCount]() { ++shortCount; });
    Require(longId != IocpPoller::InvalidTimeoutId
        && shortId != IocpPoller::InvalidTimeoutId,
        "IOCP deadline test should schedule both timers");

    const auto start = std::chrono::steady_clock::now();
    std::vector<Channel*> active;
    poller.Poll(200, active);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    poller.CancelTimeout(longId);
    Require(shortCount == 1, "Nearest IOCP deadline should fire first");
    Require(longCount == 0, "Later IOCP deadline must remain pending");
    Require(elapsed < std::chrono::milliseconds(180),
        "Nearest IOCP deadline should narrow the wait budget");
}

void TestIocpChannelReadWriteAndGeneration() {
    DatagramPair pair = MakeDatagramPair();
    IocpPoller poller(nullptr);
    Channel channel(nullptr, pair.receiver, IOEvent::Read | IOEvent::Write);
    Require(poller.AddChannel(&channel),
        "IOCP Channel should be accepted before Activate");
    const std::uint64_t firstId = poller.ChannelRegistrationId(&channel);
    Require(firstId != 0, "IOCP Channel should receive a generation id");
    Require(poller.Activate(), "IOCP Channel should arm after Activate");

    const char payload = 'x';
    Require(::sendto(
        pair.sender,
        &payload,
        1,
        0,
        reinterpret_cast<const sockaddr*>(&pair.address),
        static_cast<int>(sizeof(pair.address))) == 1,
        "IOCP Channel test datagram should send");

    std::vector<Channel*> active;
    poller.Poll(200, active);
    Require(!active.empty(), "IOCP Channel should report a network event");
    Require(HasNetworkEvent(channel.Revents(), IOEvent::Read),
        "IOCP Channel compatibility should report read readiness");

    channel.EnableWriting();
    Require(poller.UpdateChannel(&channel), "IOCP Channel update should succeed");
    const std::uint64_t secondId = poller.ChannelRegistrationId(&channel);
    Require(secondId != 0 && secondId != firstId,
        "IOCP Channel update should allocate a new generation");
    poller.PostTestChannelPacket(firstId);
    active.clear();
    poller.Poll(0, active);
    Require(active.empty(), "Old IOCP Channel packet must be ignored after update");
}

void TestIocpChannelRemoveDropsLatePacket() {
    DatagramPair pair = MakeDatagramPair();
    IocpPoller poller(nullptr);
    Require(poller.Activate(), "IOCP Channel remove test should activate");
    Channel channel(nullptr, pair.receiver, IOEvent::Read);
    Require(poller.AddChannel(&channel), "IOCP Channel remove test should add");
    const std::uint64_t registrationId = poller.ChannelRegistrationId(&channel);
    Require(poller.RemoveChannel(&channel), "IOCP Channel remove should succeed");
    poller.PostTestChannelPacket(registrationId);

    std::vector<Channel*> active;
    poller.Poll(0, active);
    Require(active.empty(), "Removed Channel must ignore late completion packet");
}

void TestIocpEventLoopPostTaskWakesPromptly() {
    EventLoop loop;
    loop.SetPollTimeout(5000);
    std::atomic<bool> taskRan{ false };
    std::thread runner([&loop]() { loop.Start(); });

    const auto startWait = std::chrono::steady_clock::now();
    while (!loop.IsRunning()
        && std::chrono::steady_clock::now() - startWait < std::chrono::seconds(2)) {
        std::this_thread::yield();
    }
    Require(loop.IsRunning(), "IOCP EventLoop should start before wakeup test");

    const auto start = std::chrono::steady_clock::now();
    loop.PostTask([&loop, &taskRan]() {
        taskRan.store(true, std::memory_order_release);
        loop.Shutdown();
    });
    runner.join();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    Require(taskRan.load(std::memory_order_acquire),
        "IOCP EventLoop should execute the posted task");
    Require(elapsed < std::chrono::milliseconds(500),
        "PostTask should wake IOCP EventLoop immediately");
}

void TestIocpConnectSuccess() {
    TcpListener listener = MakeTcpListener();
    IocpPoller poller(nullptr);
    Require(poller.Activate(), "IOCP ConnectEx success test should activate");
    int connectCount = 0;
    int connectError = -1;
    std::thread::id callbackThread;
    const std::thread::id issuerThread = std::this_thread::get_id();
    const auto connectId = poller.StartConnect(
        listener.address,
        [&connectCount, &connectError, &callbackThread](SocketType fd, int error) {
            ++connectCount;
            connectError = error;
            callbackThread = std::this_thread::get_id();
            Internal::CloseSocket(fd);
        });
    Require(connectId != IocpPoller::InvalidConnectId,
        "ConnectEx success should return a connect id");

    std::vector<Channel*> active;
    for (int attempt = 0; attempt < 10 && connectCount == 0; ++attempt) {
        poller.Poll(200, active);
    }
    Require(connectCount == 1 && connectError == 0,
        "ConnectEx should complete once on issuer thread");
    Require(callbackThread == issuerThread,
        "ConnectEx callback should run on issuer thread");
    Require(poller.GetCompletionStats().pendingConnectOperations == 0,
        "Successful ConnectEx operation should retire");
}

void TestIocpConnectRefused() {
    const LikesProgram::Net::Address refusedAddress("127.0.0.1", 0);

    IocpPoller poller(nullptr);
    Require(poller.Activate(), "IOCP ConnectEx refused test should activate");
    int callbackCount = 0;
    int callbackError = 0;
    const auto connectId = poller.StartConnect(
        refusedAddress,
        [&callbackCount, &callbackError](SocketType fd, int error) {
            ++callbackCount;
            callbackError = error;
            Require(fd == LikesProgram::Net::kInvalidSocket,
                "Refused ConnectEx callback must not expose a socket");
        });
    Require(connectId != IocpPoller::InvalidConnectId,
        "Refused ConnectEx should still return an accepted id");

    std::vector<Channel*> active;
    for (int attempt = 0; attempt < 10 && callbackCount == 0; ++attempt) {
        poller.Poll(200, active);
    }
    if (callbackCount != 1 || callbackError == 0) {
        std::cerr << "refused callbackCount=" << callbackCount
            << " error=" << callbackError
            << " pending=" << poller.GetCompletionStats().pendingConnectOperations
            << std::endl;
    }
    Require(callbackCount == 1 && callbackError != 0,
        "Refused ConnectEx should complete with an error once");
    Require(poller.GetCompletionStats().pendingConnectOperations == 0,
        "Refused ConnectEx operation should retire");
}

void TestIocpConnectCancelSuppressesCallback() {
    TcpListener listener = MakeTcpListener();
    IocpPoller poller(nullptr);
    Require(poller.Activate(), "IOCP ConnectEx cancel test should activate");
    int callbackCount = 0;
    const auto connectId = poller.StartConnect(
        listener.address,
        [&callbackCount](SocketType, int) { ++callbackCount; });
    Require(connectId != IocpPoller::InvalidConnectId,
        "Canceled ConnectEx should return an accepted id");
    poller.CancelConnect(connectId);

    std::vector<Channel*> active;
    for (int attempt = 0;
        attempt < 10 && poller.GetCompletionStats().pendingConnectOperations != 0;
        ++attempt) {
        poller.Poll(50, active);
    }
    Require(callbackCount == 0, "CancelConnect should suppress user callback");
    Require(poller.GetCompletionStats().pendingConnectOperations == 0,
        "Canceled ConnectEx operation should retire after terminal packet");
}

void TestIocpConnectGenerationReuse() {
    TcpListener listener = MakeTcpListener();
    IocpPoller poller(nullptr);
    Require(poller.Activate(), "IOCP ConnectEx reuse test should activate");
    int canceledCount = 0;
    const auto canceledId = poller.StartConnect(
        listener.address,
        [&canceledCount](SocketType, int) { ++canceledCount; });
    Require(canceledId != IocpPoller::InvalidConnectId,
        "First ConnectEx should return an id");
    poller.CancelConnect(canceledId);
    std::vector<Channel*> active;
    for (int attempt = 0;
        attempt < 10 && poller.GetCompletionStats().pendingConnectOperations != 0;
        ++attempt) {
        poller.Poll(20, active);
    }
    Require(canceledCount == 0, "Canceled generation must not callback");

    int successCount = 0;
    const auto reusedId = poller.StartConnect(
        listener.address,
        [&successCount](SocketType fd, int error) {
            if (error == 0) ++successCount;
            Internal::CloseSocket(fd);
        });
    Require(reusedId != IocpPoller::InvalidConnectId && reusedId != canceledId,
        "ConnectEx socket reuse must allocate a new generation id");
    for (int attempt = 0; attempt < 10 && successCount == 0; ++attempt) {
        poller.Poll(200, active);
    }
    Require(successCount == 1, "Reused ConnectEx generation should complete once");
}

void TestIocpClientConnectTransfersCompletionAssociation() {
    Server server(
        LikesProgram::Net::Address("127.0.0.1", 0),
        LikesProgram::Net::TransportKind::Tcp,
        [](SocketType fd, EventLoop* loop) {
            return std::make_shared<LikesProgram::Net::Connection>(fd, loop);
        });
    server.SetWorkerThreads(2);
    server.Start();
    const auto addresses = server.GetListenAddresses();
    Require(!addresses.empty() && addresses.front().Port() != 0,
        "IOCP Client transfer test should expose a bound listener");

    LikesProgram::Net::Client client(
        addresses.front(),
        LikesProgram::Net::TransportKind::Tcp,
        [](SocketType fd, EventLoop* loop) {
            return std::make_shared<LikesProgram::Net::Connection>(fd, loop);
        });
    client.Start();
    Require(client.GetConnection() != nullptr,
        "IOCP Client should publish a Connection after ConnectEx completion");

    client.Shutdown();
    server.Shutdown();
}

// 连续 AcceptEx 必须保持单 issuer，并在用户回调前补交 replacement。
void TestIocpAcceptThreeClients() {
    TcpListener listener = MakeTcpListener();
    IocpPoller poller(nullptr);
    Require(poller.Activate(), "IOCP AcceptEx test should activate");
    int acceptedCount = 0;
    std::thread::id callbackThread;
    const std::thread::id issuerThread = std::this_thread::get_id();
    Require(poller.StartAccept(
        listener.fd,
        [&acceptedCount, &callbackThread](SocketType fd) {
            ++acceptedCount;
            callbackThread = std::this_thread::get_id();
            Internal::CloseSocket(fd);
        }),
        "IOCP AcceptEx should start");

    std::vector<SocketType> clients;
    clients.push_back(ConnectTcpClient(listener.address));
    clients.push_back(ConnectTcpClient(listener.address));
    clients.push_back(ConnectTcpClient(listener.address));
    std::vector<Channel*> active;
    for (int attempt = 0; attempt < 20 && acceptedCount < 3; ++attempt) {
        poller.Poll(100, active);
    }
    for (SocketType client : clients) Internal::CloseSocket(client);
    Require(acceptedCount == 3, "AcceptEx should deliver three accepted sockets");
    Require(callbackThread == issuerThread,
        "AcceptEx callback should run on the stable issuer thread");
    Require(poller.AcceptSubmissionCount() >= 4,
        "AcceptEx should submit the replacement before callback");
    poller.StopAccept(listener.fd);
    poller.Poll(100, active);
}

// StopAccept 必须等待 terminal packet，同时抑制所有迟到用户回调。
void TestIocpStopAcceptSuppressesLateCallback() {
    TcpListener listener = MakeTcpListener();
    IocpPoller poller(nullptr);
    Require(poller.Activate(), "IOCP StopAccept test should activate");
    int lateAcceptCount = 0;
    Require(poller.StartAccept(
        listener.fd,
        [&lateAcceptCount](SocketType fd) {
            ++lateAcceptCount;
            Internal::CloseSocket(fd);
        }),
        "IOCP StopAccept test should start");
    poller.StopAccept(listener.fd);
    const SocketType client = ConnectTcpClient(listener.address);
    std::vector<Channel*> active;
    for (int attempt = 0; attempt < 10; ++attempt) poller.Poll(50, active);
    Internal::CloseSocket(client);
    Require(lateAcceptCount == 0,
        "StopAccept should suppress late user callback");
}

// 用户 accept callback 异常不能击穿 Poller 或泄漏 accepted socket。
void TestIocpAcceptCallbackExceptionIsolated() {
    TcpListener listener = MakeTcpListener();
    IocpPoller poller(nullptr);
    Require(poller.Activate(), "IOCP accept exception test should activate");
    int callbackCount = 0;
    Require(poller.StartAccept(
        listener.fd,
        [&callbackCount](SocketType) {
            ++callbackCount;
            throw std::runtime_error("accept callback failure");
        }),
        "IOCP accept exception test should start");
    const SocketType client = ConnectTcpClient(listener.address);
    std::vector<Channel*> active;
    for (int attempt = 0; attempt < 10 && callbackCount == 0; ++attempt) {
        poller.Poll(100, active);
    }
    Internal::CloseSocket(client);
    Require(callbackCount == 1,
        "AcceptEx callback exception should not skip delivery");
    poller.StopAccept(listener.fd);
    poller.Poll(100, active);
}

// Windows 单 listener 必须把 accepted socket 轮询投递到目标 worker。
void TestIocpServerAdoptsAcceptedSocketsAcrossWorkers() {
    std::mutex callbackMutex; // 保护 worker 回调线程快照
    std::condition_variable callbackCv; // 等待多个 accepted fd 到达目标 worker
    std::size_t factoryCount = 0; // 已在目标 worker 执行的 factory 次数
    bool allCallbacksOnLoopThread = true; // 每次 factory 是否位于对应 EventLoop 线程
    std::set<std::thread::id> callbackThreads; // 实际承接连接的 worker 线程集合
    std::set<EventLoop*> callbackLoops; // 实际承接连接的 worker loop 集合

    LikesProgram::Net::Server server(
        LikesProgram::Net::Address("127.0.0.1", 0),
        [&callbackMutex,
            &callbackCv,
            &factoryCount,
            &allCallbacksOnLoopThread,
            &callbackThreads,
            &callbackLoops](SocketType, EventLoop* loop) {
            {
                std::lock_guard<std::mutex> lock(callbackMutex); // 发布一次 worker adoption 观察
                ++factoryCount;
                allCallbacksOnLoopThread = allCallbacksOnLoopThread
                    && loop != nullptr
                    && loop->IsInLoopThread();
                callbackThreads.insert(std::this_thread::get_id());
                callbackLoops.insert(loop);
            }
            callbackCv.notify_all();
            return std::shared_ptr<LikesProgram::Net::Connection>{};
        });
    server.SetWorkerThreads(2);
    server.Start();

    const auto addresses = server.GetListenAddresses(); // 读取单 listener 的实际端口
    Require(!addresses.empty() && addresses.front().Port() != 0,
        "IOCP Server adoption should expose a bound address");

    std::vector<SocketType> clients; // 保持连接直到 accept 回调完成
    clients.reserve(4);
    for (int index = 0; index < 4; ++index) {
        clients.push_back(ConnectTcpClient(addresses.front()));
    }

    {
        std::unique_lock<std::mutex> lock(callbackMutex); // 等待所有连接完成 worker adoption
        const bool completed = callbackCv.wait_for(
            lock,
            std::chrono::seconds(2),
            [&factoryCount]() { return factoryCount >= 4; });
        Require(completed, "IOCP Server should adopt all accepted sockets");
        Require(allCallbacksOnLoopThread,
            "IOCP Server adoption should construct on the target worker loop");
        Require(callbackThreads.size() >= 2,
            "IOCP Server should distribute accepted sockets across workers");
        Require(callbackLoops.size() >= 2,
            "IOCP Server should select at least two target worker loops");
    }

    for (const auto client : clients) Internal::CloseSocket(client);
    server.Shutdown();
}

// Windows owning TCP completion 必须覆盖启动、双向 payload、写完成与 EOF。
void TestIocpPlainTcpReadWriteAndPeerEof() {
    TcpListener listener = MakeTcpListener();
    const SocketType peer = ConnectTcpClient(listener.address); // 测试线程持有的对端 socket
    const SocketType connectionFd = Internal::AcceptSocket(listener.fd, nullptr, nullptr); // Connection 接管端
    Require(connectionFd != LikesProgram::Net::kInvalidSocket,
        "IOCP TCP connection should accept a loopback peer");
    Require(Internal::SetNonBlocking(peer),
        "IOCP TCP peer should enter nonblocking mode");

    EventLoop loop;
    IocpTcpProbe probe; // 跨线程观察 owning completion 顺序
    auto connection = std::make_shared<IocpPlainConnection>(connectionFd, &loop, probe);
    loop.AttachConnection(connection);
    loop.PostTask([connection]() { connection->Start(); });
    std::thread worker([&loop]() { loop.Start(); });

    const bool connected = WaitUntil(
        [&probe]() { return probe.connected.load(std::memory_order_acquire); },
        std::chrono::seconds(2));
    std::string outbound; // 从 WSASend completion 对端读取的 payload
    if (connected) {
        std::array<char, 32> buffer{}; // 非阻塞读取的小型稳定缓冲
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (outbound.size() < 8 && std::chrono::steady_clock::now() < deadline) {
            const std::int64_t received = Internal::ReceiveSocket(
                peer,
                buffer.data(),
                buffer.size(),
                0); // 读取 Connection 启动回调提交的业务数据
            if (received > 0) {
                outbound.append(buffer.data(), static_cast<std::size_t>(received));
            }
            else if (received < 0 && !Internal::IsWouldBlock(Internal::GetLastSocketError())) {
                break;
            }
            else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }

    const char inbound[] = "inbound"; // 触发 owning WSARecv completion
    const std::int64_t sent = connected
        ? Internal::SendSocket(peer, inbound, sizeof(inbound) - 1, 0)
        : -1;
    const bool inboundReceived = sent == static_cast<std::int64_t>(sizeof(inbound) - 1)
        && WaitUntil(
            [&probe]() { return probe.messageReceived.load(std::memory_order_acquire); },
            std::chrono::seconds(2));
    if (connected) (void)Internal::ShutdownWrite(peer);
    const bool peerClosed = WaitUntil(
        [&probe]() { return probe.closed.load(std::memory_order_acquire); },
        std::chrono::seconds(2));

    loop.Shutdown();
    worker.join();
    Internal::CloseSocket(peer);

    std::string receivedPayload; // 锁内复制业务回调最终快照
    {
        std::lock_guard<std::mutex> lock(probe.payloadMutex);
        receivedPayload = probe.payload;
    }
    Require(connected, "IOCP StartConnection should enter OnConnected");
    Require(outbound == "outbound", "IOCP WSASend should preserve outbound payload");
    Require(probe.writeCompleted.load(std::memory_order_acquire),
        "IOCP WSASend should notify one completed write chain");
    Require(inboundReceived && receivedPayload == "inbound",
        "IOCP WSARecv should preserve inbound payload");
    Require(peerClosed && probe.error.load(std::memory_order_acquire) == 0,
        "IOCP zero-byte read should close as peer EOF without an error");
}

void TestIocpTcpPartialWriteAndPauseResume() {
    TcpListener listener = MakeTcpListener();
    const SocketType peer = ConnectTcpClient(listener.address); // 暂不读取，制造发送背压
    const SocketType connectionFd = Internal::AcceptSocket(listener.fd, nullptr, nullptr);
    Require(connectionFd != LikesProgram::Net::kInvalidSocket,
        "IOCP partial TCP connection should accept a loopback peer");
    Require(Internal::SetNonBlocking(peer),
        "IOCP partial TCP peer should enter nonblocking mode");

    int sendBuffer = 1024; // 缩小发送窗口，迫使大 Buffer 经过多个 completion 片段
    (void)::setsockopt(
        connectionFd,
        SOL_SOCKET,
        SO_SNDBUF,
        reinterpret_cast<const char*>(&sendBuffer),
        static_cast<int>(sizeof(sendBuffer)));

    EventLoop loop;
    IocpTcpProbe probe; // 复用同一 owning Connection 观察器
    probe.largeWrite = true;
    probe.pauseOnConnect = true;
    probe.expectedOutbound.assign(4 * 1024 * 1024, 'x'); // 足够超过单次发送窗口
    auto connection = std::make_shared<IocpPlainConnection>(connectionFd, &loop, probe);
    loop.AttachConnection(connection);
    loop.PostTask([connection]() { connection->Start(); });
    std::thread worker([&loop]() { loop.Start(); });

    Require(WaitUntil(
        [&probe]() { return probe.connected.load(std::memory_order_acquire); },
        std::chrono::seconds(2)),
        "IOCP partial TCP connection should start");
    const char pausedPayload[] = "paused"; // PauseReading 后先发送一段数据
    Require(Internal::SendSocket(
        peer,
        pausedPayload,
        sizeof(pausedPayload) - 1,
        0) == static_cast<std::int64_t>(sizeof(pausedPayload) - 1),
        "IOCP paused peer should send payload");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    Require(!probe.messageReceived.load(std::memory_order_acquire),
        "IOCP PauseReading should suppress owning read callback");

    connection->ResumeReading();
    Require(WaitUntil(
        [&probe]() { return probe.messageReceived.load(std::memory_order_acquire); },
        std::chrono::seconds(2)),
        "IOCP ResumeReading should resubmit owning read");

    std::string outbound; // 恢复读取对端，验证大写链顺序与完整性
    std::vector<char> buffer(64 * 1024); // 大块读取缓冲放在堆上，避免 MSVC 栈上限告警
    const auto readDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (outbound.size() < probe.expectedOutbound.size()
        && std::chrono::steady_clock::now() < readDeadline) {
        const std::int64_t received = Internal::ReceiveSocket(
            peer,
            buffer.data(),
            buffer.size(),
            0);
        if (received > 0) {
            outbound.append(buffer.data(), static_cast<std::size_t>(received));
        }
        else if (received < 0 && !Internal::IsWouldBlock(Internal::GetLastSocketError())) {
            break;
        }
        else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    Require(outbound == probe.expectedOutbound,
        "IOCP partial WSASend should preserve the full BufferChain order");
    Require(WaitUntil(
        [&probe]() { return probe.writeCompleted.load(std::memory_order_acquire); },
        std::chrono::seconds(2)),
        "IOCP partial WSASend should notify completion after the final segment");

    (void)Internal::ShutdownWrite(peer);
    Require(WaitUntil(
        [&probe]() { return probe.closed.load(std::memory_order_acquire); },
        std::chrono::seconds(2)),
        "IOCP partial TCP peer EOF should still close the Connection");
    loop.Shutdown();
    worker.join();
    Internal::CloseSocket(peer);
}

void TestIocpTlsCloseNotifyCompletion() {
    TcpListener listener = MakeTcpListener();
    const SocketType peer = ConnectTcpClient(listener.address);
    const SocketType connectionFd = Internal::AcceptSocket(listener.fd, nullptr, nullptr);
    Require(connectionFd != LikesProgram::Net::kInvalidSocket,
        "IOCP TLS connection should accept a loopback peer");
    Require(Internal::SetNonBlocking(peer),
        "IOCP TLS peer should enter nonblocking mode");

    EventLoop loop;
    IocpTlsProbe probe; // 内存 Engine 只验证 Net 公共 TLS 契约
    auto connection = std::make_shared<IocpTlsConnection>(connectionFd, &loop, probe);
    loop.AttachConnection(connection);
    loop.PostTask([connection]() { connection->Start(); });
    std::thread worker([&loop]() { loop.Start(); });

    Require(WaitUntil(
        [&probe]() { return probe.handshakeDone.load(std::memory_order_acquire); },
        std::chrono::seconds(2)),
        "IOCP TLS Engine should reach Active on the owning connection");

    std::string outbound; // 先观察应用密文，再观察唯一 close_notify
    std::array<char, 64> buffer{};
    const auto appDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (outbound.find("tls:app") == std::string::npos
        && std::chrono::steady_clock::now() < appDeadline) {
        const std::int64_t received = Internal::ReceiveSocket(
            peer,
            buffer.data(),
            buffer.size(),
            0);
        if (received > 0) outbound.append(buffer.data(), static_cast<std::size_t>(received));
        else if (received < 0 && !Internal::IsWouldBlock(Internal::GetLastSocketError())) break;
        else std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Require(outbound.find("tls:app") != std::string::npos,
        "IOCP TLS application plaintext should enter the owning write chain");

    connection->Shutdown();
    const auto closeDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (outbound.find("close") == std::string::npos
        && std::chrono::steady_clock::now() < closeDeadline) {
        const std::int64_t received = Internal::ReceiveSocket(
            peer,
            buffer.data(),
            buffer.size(),
            0);
        if (received > 0) outbound.append(buffer.data(), static_cast<std::size_t>(received));
        else if (received < 0 && !Internal::IsWouldBlock(Internal::GetLastSocketError())) break;
        else std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Require(outbound.find("close") != std::string::npos,
        "IOCP TLS Shutdown should write close_notify before transport close");
    Require(WaitUntil(
        [&probe]() { return probe.closed.load(std::memory_order_acquire); },
        std::chrono::seconds(2)),
        "IOCP TLS close_notify completion should close the connection");
    Require(probe.error.load(std::memory_order_acquire) == 0,
        "IOCP TLS close_notify should not report a transport error");

    loop.Shutdown();
    worker.join();
    Internal::CloseSocket(peer);
}

void TestIocpUdpDatagramMetadataFifoAndZeroLength() {
    DatagramPair pair = MakeDatagramPair();
    SocketType secondPeer = Internal::CreateSocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    Require(secondPeer != LikesProgram::Net::kInvalidSocket,
        "IOCP UDP second peer should open");
    sockaddr_in secondAddress{};
    secondAddress.sin_family = AF_INET;
    secondAddress.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    secondAddress.sin_port = 0;
    Require(Internal::BindSocket(
        secondPeer,
        reinterpret_cast<const sockaddr*>(&secondAddress),
        static_cast<SocketLength>(sizeof(secondAddress))) == 0,
        "IOCP UDP second peer should bind");
    Require(Internal::SetNonBlocking(secondPeer),
        "IOCP UDP second peer should enter nonblocking mode");

    sockaddr_storage receiverStorage{}; // 显式 SendTo 目标地址
    std::memcpy(&receiverStorage, &pair.address, sizeof(pair.address));
    const LikesProgram::Net::Address receiverAddress(
        receiverStorage,
        static_cast<SocketLength>(sizeof(pair.address)));

    EventLoop loop;
    IocpUdpProbe probe; // 观察 UDP peer/截断/FIFO 语义
    auto connection = std::make_shared<IocpUdpConnection>(pair.receiver, &loop, probe);
    loop.AttachConnection(connection);
    loop.PostTask([connection]() { connection->Start(); });
    std::thread worker([&loop]() { loop.Start(); });
    Require(WaitUntil(
        [&probe]() { return probe.connected.load(std::memory_order_acquire); },
        std::chrono::seconds(2)),
        "IOCP UDP Connection should start owning recvfrom");

    const char first[] = "abcdef"; // 6-byte wire payload，业务容量为 4
    Require(Internal::SendSocketTo(
        pair.sender,
        first,
        sizeof(first) - 1,
        0,
        receiverAddress.SockAddr(),
        receiverAddress.Length()) == static_cast<std::int64_t>(sizeof(first) - 1),
        "IOCP UDP first peer should send payload");
    Require(Internal::SendSocketTo(
        secondPeer,
        "peer2",
        5,
        0,
        receiverAddress.SockAddr(),
        receiverAddress.Length()) == 5,
        "IOCP UDP second peer should send payload");
    Require(Internal::SendSocketTo(
        pair.sender,
        nullptr,
        0,
        0,
        receiverAddress.SockAddr(),
        receiverAddress.Length()) == 0,
        "IOCP UDP peer should send a zero-length datagram");

    Require(WaitUntil(
        [&probe]() {
            std::lock_guard<std::mutex> lock(probe.mutex);
            return probe.payloads.size() >= 3;
        },
        std::chrono::seconds(2)),
        "IOCP UDP should deliver FIFO datagrams from multiple peers");

    std::vector<std::string> payloads; // 锁内复制回调结果
    std::vector<std::size_t> originalBytes;
    std::vector<bool> truncated;
    std::vector<std::string> peers;
    {
        std::lock_guard<std::mutex> lock(probe.mutex);
        payloads = probe.payloads;
        originalBytes = probe.originalBytes;
        truncated = probe.truncated;
        peers = probe.peers;
    }
    Require(payloads[0] == "abcd" && originalBytes[0] == 6 && truncated[0],
        "IOCP UDP should preserve exact original length and truncate business payload");
    Require(payloads[1] == "peer" && originalBytes[1] == 5 && truncated[1],
        "IOCP UDP should preserve the second peer metadata independently");
    Require(payloads[2].empty() && originalBytes[2] == 0 && !truncated[2],
        "IOCP UDP should deliver zero-length datagrams exactly once");
    Require(peers[0] != peers[1],
        "IOCP UDP peer metadata should distinguish independent senders");

    const LikesProgram::Net::Address senderAddress =
        LikesProgram::Net::Address::GetLocalAddress(pair.sender);
    Require(senderAddress.IsValid(), "IOCP UDP sender should expose an ephemeral address");
    connection->SendTo(senderAddress, "reply", 5);
    connection->SendTo(senderAddress, nullptr, 0); // FIFO 中的零长度 outbound datagram

    std::vector<std::string> replies; // 从 sender socket 读取 connection 的 FIFO 输出
    std::array<char, 32> replyBuffer{};
    const auto replyDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (replies.size() < 2 && std::chrono::steady_clock::now() < replyDeadline) {
        const std::int64_t received = Internal::ReceiveSocket(
            pair.sender,
            replyBuffer.data(),
            replyBuffer.size(),
            0);
        if (received >= 0) {
            replies.emplace_back(
                replyBuffer.data(),
                static_cast<std::size_t>(received));
        }
        else if (!Internal::IsWouldBlock(Internal::GetLastSocketError())) {
            break;
        }
        else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    Require(replies.size() == 2 && replies[0] == "reply" && replies[1].empty(),
        "IOCP UDP send FIFO should preserve payload and zero-length datagrams");
    Require(WaitUntil(
        [&probe]() { return probe.writeCompleted.load(std::memory_order_acquire); },
        std::chrono::seconds(2)),
        "IOCP UDP FIFO should publish one final write completion");

    const CompletionStats stats = loop.GetCompletionStats();
    Require(stats.datagramReceiveCompletions >= 3
        && stats.datagramSendCompletions >= 2
        && stats.pendingDatagramSends == 0,
        "IOCP UDP completion stats should converge after FIFO drain");
    connection->ForceClose();
    Require(WaitUntil(
        [&probe]() { return probe.closed.load(std::memory_order_acquire); },
        std::chrono::seconds(2)),
        "IOCP UDP ForceClose should finish after read cancellation");
    loop.Shutdown();
    worker.join();
    Internal::CloseSocket(secondPeer);
}

void TestIocpForceCloseDrainsPendingOperations() {
    TcpListener listener = MakeTcpListener();
    const SocketType peer = ConnectTcpClient(listener.address);
    const SocketType connectionFd = Internal::AcceptSocket(listener.fd, nullptr, nullptr);
    Require(connectionFd != LikesProgram::Net::kInvalidSocket,
        "IOCP ForceClose connection should accept a loopback peer");
    Require(Internal::SetNonBlocking(peer),
        "IOCP ForceClose peer should enter nonblocking mode");

    int sendBuffer = 1024; // 保证 write operation 仍在 pending 状态
    (void)::setsockopt(
        connectionFd,
        SOL_SOCKET,
        SO_SNDBUF,
        reinterpret_cast<const char*>(&sendBuffer),
        static_cast<int>(sizeof(sendBuffer)));

    EventLoop loop;
    IocpTcpProbe probe; // 关闭时只允许 OnClosed，不允许伪造 late error
    probe.largeWrite = true;
    probe.expectedOutbound.assign(8 * 1024 * 1024, 'z');
    auto connection = std::make_shared<IocpPlainConnection>(connectionFd, &loop, probe);
    loop.AttachConnection(connection);
    loop.PostTask([connection]() { connection->Start(); });
    std::thread worker([&loop]() { loop.Start(); });

    Require(WaitUntil(
        [&probe]() { return probe.connected.load(std::memory_order_acquire); },
        std::chrono::seconds(2)),
        "IOCP ForceClose connection should start before cancellation");
    connection->ForceClose();
    Require(WaitUntil(
        [&probe]() { return probe.closed.load(std::memory_order_acquire); },
        std::chrono::seconds(2)),
        "IOCP ForceClose should publish Closed while terminal packets drain");
    loop.Shutdown();
    worker.join();
    Require(probe.error.load(std::memory_order_acquire) == 0,
        "IOCP ForceClose cancellation should not publish a late connection error");
    Internal::CloseSocket(peer);
}

} // namespace

int main() {
    TestIocpActivateCreatesCompletionPort();
    TestIocpPollConsumesPostedPacket();
    TestIocpTimeoutFiresExactlyOnce();
    TestIocpTimeoutCancelSuppressesCallback();
    TestIocpTimeoutUsesNearestDeadline();
    TestIocpChannelReadWriteAndGeneration();
    TestIocpChannelRemoveDropsLatePacket();
    TestIocpEventLoopPostTaskWakesPromptly();
    TestIocpConnectSuccess();
    TestIocpConnectRefused();
    TestIocpConnectCancelSuppressesCallback();
    TestIocpConnectGenerationReuse();
    TestIocpClientConnectTransfersCompletionAssociation();
    TestIocpAcceptThreeClients();
    TestIocpStopAcceptSuppressesLateCallback();
    TestIocpAcceptCallbackExceptionIsolated();
    TestIocpServerAdoptsAcceptedSocketsAcrossWorkers();
    TestIocpPlainTcpReadWriteAndPeerEof();
    TestIocpTcpPartialWriteAndPauseResume();
    TestIocpTlsCloseNotifyCompletion();
    TestIocpUdpDatagramMetadataFifoAndZeroLength();
    TestIocpForceCloseDrainsPendingOperations();
    return 0;
}
