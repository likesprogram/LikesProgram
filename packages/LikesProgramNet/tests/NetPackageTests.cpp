#include <LikesProgram/Net/Net.hpp>
#include <LikesProgram/Core/Version.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace {
    // Net 回归测试覆盖基础缓冲、地址解析、本地 TCP/UDP 往返和安全传输扩展接口。
    void Require(bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    }

    std::unique_ptr<LikesProgram::Net::Transport> MakePlainTransport(
        LikesProgram::Net::SocketType fd,
        LikesProgram::Net::TransportKind kind) {
        // 测试用工厂显式创建 transport，确保 Server/Client 的传输选择路径可组合。
        if (kind == LikesProgram::Net::TransportKind::Tcp) {
            return std::make_unique<LikesProgram::Net::TcpTransport>(fd);
        }

        if (kind == LikesProgram::Net::TransportKind::Udp) {
            return std::make_unique<LikesProgram::Net::UdpTransport>(fd);
        }

        return {};
    }

    class EchoConnection final : public LikesProgram::Net::Connection {
    public:
        // 复用基础连接构造，允许测试为 TCP/UDP 注入不同 transport。
        EchoConnection(
            LikesProgram::Net::SocketType fd,
            LikesProgram::Net::EventLoop* loop,
            std::unique_ptr<LikesProgram::Net::Transport> transport = nullptr)
            : Connection(fd, loop, std::move(transport)) {
        }

    protected:
        // 收到任意数据后原样写回，并消费输入缓冲。
        void OnMessage(LikesProgram::Net::Buffer& in) override {
            Send(in.Peek(), in.ReadableBytes());
            in.RetrieveAll();
        }
    };

    class CaptureConnection final : public LikesProgram::Net::Connection {
    public:
        // 保存测试断言需要共享的接收状态。
        CaptureConnection(
            LikesProgram::Net::SocketType fd,
            LikesProgram::Net::EventLoop* loop,
            std::atomic<bool>& received,
            std::string& payload,
            std::unique_ptr<LikesProgram::Net::Transport> transport = nullptr)
            : Connection(fd, loop, std::move(transport)),
            m_received(received),
            m_payload(payload) {
        }

    protected:
        // 收到回显后记录文本并关闭连接。
        void OnMessage(LikesProgram::Net::Buffer& in) override {
            m_payload.assign(in.AsStringView());
            in.RetrieveAll();
            m_received.store(true, std::memory_order_release);
            ForceClose();
        }

    private:
        std::atomic<bool>& m_received; // 测试线程观察的接收完成标记
        std::string& m_payload;        // 测试线程读取的回显内容
    };

    class ImmediateUpgradeConnection final : public LikesProgram::Net::Connection {
    public:
        // 用于验证连接建立后可由虚事件立即切入安全层握手。
        ImmediateUpgradeConnection(
            LikesProgram::Net::SocketType fd,
            LikesProgram::Net::EventLoop* loop,
            std::unique_ptr<LikesProgram::Net::Transport> transport,
            std::atomic<int>& handshakes)
            : Connection(fd, loop, std::move(transport)),
            m_handshakes(handshakes) {
        }

    protected:
        // 安全层资源准备完成后立即请求升级，模拟连接后立刻 TLS/SSL。
        void OnSecureLayerReady() override {
            UpgradeCommunication();
        }

        // 握手完成后记录次数，证明业务读写前已经进入安全态。
        void OnHandshakeDone() override {
            m_handshakes.fetch_add(1, std::memory_order_acq_rel);
        }

        // 收到数据后回显，验证握手完成后仍能保留通信能力。
        void OnMessage(LikesProgram::Net::Buffer& in) override {
            Send(in.Peek(), in.ReadableBytes());
            in.RetrieveAll();
        }

    private:
        std::atomic<int>& m_handshakes; // 测试线程观察的握手完成次数
    };

    template <typename BaseTransport>
    class UserSecureTransportBase final : public BaseTransport {
    public:
        // 测试用安全层扩展子类不链接任何 TLS 库，只验证继承契约可用。
        explicit UserSecureTransportBase(LikesProgram::Net::SocketType fd)
            : BaseTransport(fd) {
        }

        // 测试实现记录安全层初始化入口是否被调用。
        LikesProgram::Net::IoResult InitializeSecureLayer() override {
            m_initialized = true;
            return LikesProgram::Net::IoResult{ LikesProgram::Net::IoStatus::Ok, 0, 0 };
        }

        // 测试实现记录通信升级入口是否被调用。
        LikesProgram::Net::IoResult UpgradeCommunication() override {
            m_upgraded = true;
            this->BeginSecureHandshake();
            return LikesProgram::Net::IoResult{ LikesProgram::Net::IoStatus::Ok, 0, 0 };
        }

        // 测试实现将一次握手推进映射为状态切换，模拟 TLS/SSL 完成。
        LikesProgram::Net::IoResult Handshake() override {
            ++m_handshakeCount;
            this->CompleteSecureHandshake();
            return LikesProgram::Net::IoResult{ LikesProgram::Net::IoStatus::Ok, 0, 0 };
        }

        // 测试实现默认关注读事件。
        bool RemainWantRead() const override {
            return true;
        }

        // 测试实现不需要写事件推进握手。
        bool RemainWantWrite() const override {
            return false;
        }

        bool HandshakeCompleted() const noexcept {
            return m_handshakeCount > 0;
        }

        bool Initialized() const noexcept {
            return m_initialized;
        }

        bool Upgraded() const noexcept {
            return m_upgraded;
        }

        bool SocketWriteUsed() const noexcept {
            return m_socketWriteCount > 0;
        }

        bool SecureWriteUsed() const noexcept {
            return m_secureWriteCount > 0;
        }

    protected:
        // 测试实现不执行真实普通 socket 读取，避免依赖无效 socket。
        LikesProgram::Net::IoResult ReadSocketSome(LikesProgram::Net::Buffer&) override {
            return LikesProgram::Net::IoResult{ LikesProgram::Net::IoStatus::WouldBlock, 0, 0 };
        }

        // 测试实现假装普通 socket 写入全部字节。
        LikesProgram::Net::IoResult WriteSocketSome(const std::uint8_t*, std::size_t len) override {
            ++m_socketWriteCount;
            return LikesProgram::Net::IoResult{
                LikesProgram::Net::IoStatus::Ok,
                static_cast<std::int64_t>(len),
                0
            };
        }

        // 测试实现假装安全层写入全部字节。
        LikesProgram::Net::IoResult WriteSecureSome(const std::uint8_t*, std::size_t len) override {
            ++m_secureWriteCount;
            return LikesProgram::Net::IoResult{
                LikesProgram::Net::IoStatus::Ok,
                static_cast<std::int64_t>(len),
                0
            };
        }

        // 测试实现无需关闭写方向。
        void ShutdownSocketWrite() override {
        }

        // 测试实现只分离无效 socket，不触碰第三方资源。
        void CloseSocket() override {
            (void)this->DetachFd();
        }

    private:
        bool m_initialized = false; // 记录初始化钩子的调用状态
        bool m_upgraded = false;    // 记录升级钩子的调用状态
        int m_handshakeCount = 0;   // 记录握手推进次数
        int m_socketWriteCount = 0; // 记录普通 socket 写钩子次数
        int m_secureWriteCount = 0; // 记录安全层写钩子次数
    };

    using UserSecureTcpTransport = UserSecureTransportBase<LikesProgram::Net::TcpTransport>;
    using UserSecureUdpTransport = UserSecureTransportBase<LikesProgram::Net::UdpTransport>;

    class StartTlsLikeTcpTransport final : public LikesProgram::Net::TcpTransport {
    public:
        // 共享配置模拟 SSL_CTX/证书链等昂贵资源，连接级初始化只持有引用。
        struct SharedSecureContext {
            std::atomic<int> loadCount{ 0 }; // 共享资源加载次数，测试中必须保持一次
        };

        StartTlsLikeTcpTransport(
            LikesProgram::Net::SocketType fd,
            std::shared_ptr<SharedSecureContext> context)
            : TcpTransport(fd),
            m_context(std::move(context)) {
        }

        // 初始化连接级安全会话，不加载共享证书，也不进入握手态。
        LikesProgram::Net::IoResult InitializeSecureLayer() override {
            ++m_sessionInitCount;
            return LikesProgram::Net::IoResult{ LikesProgram::Net::IoStatus::Ok, 0, 0 };
        }

        // 显式升级才进入握手态，用于 STARTTLS 或连接后立即 TLS。
        LikesProgram::Net::IoResult UpgradeCommunication() override {
            ++m_upgradeCount;
            BeginSecureHandshake();
            return LikesProgram::Net::IoResult{ LikesProgram::Net::IoStatus::Ok, 0, 0 };
        }

        // 测试握手一步完成，真实 TLS 可在这里处理 WANT_READ/WANT_WRITE。
        LikesProgram::Net::IoResult Handshake() override {
            ++m_handshakeCount;
            CompleteSecureHandshake();
            return LikesProgram::Net::IoResult{ LikesProgram::Net::IoStatus::Ok, 0, 0 };
        }

        int SessionInitCount() const noexcept {
            return m_sessionInitCount;
        }

        int UpgradeCount() const noexcept {
            return m_upgradeCount;
        }

        int HandshakeCount() const noexcept {
            return m_handshakeCount;
        }

        std::shared_ptr<SharedSecureContext> SharedContext() const {
            return m_context;
        }

    private:
        std::shared_ptr<SharedSecureContext> m_context; // 模拟外部共享 TLS 上下文
        int m_sessionInitCount = 0;                     // 每连接会话初始化次数
        int m_upgradeCount = 0;                         // 显式升级次数
        int m_handshakeCount = 0;                       // 握手推进次数
    };

    struct SharedSecureFactoryState {
        std::atomic<int> sharedInitCount{ 0 }; // 共享资源初始化次数
        std::atomic<int> connectionCreateCount{ 0 }; // 连接创建次数
    };

    void TestPackageIdentity() {
        const char* packageName = LikesProgram::Net::PackageName();
        const char* packageVersion = LikesProgram::Net::PackageVersion();

        Require(LikesProgram::Net::PackageAvailable(), "Net package should be available");
        Require(std::strcmp(packageName, "LikesProgramNet") == 0, "Net package name mismatch");
        Require(std::strcmp(packageVersion, LikesProgram::Version::CurrentString().data()) == 0,
            "Net package version should follow Core version");
    }

    void TestBuffer() {
        LikesProgram::Net::Buffer buffer;
        const char* text = "hello";

        buffer.Append(text, std::strlen(text));
        Require(buffer.ReadableBytes() == 5, "Buffer readable bytes mismatch");
        Require(buffer.AsStringView() == "hello", "Buffer string view mismatch");

        buffer.Consume(2);
        Require(buffer.AsStringView() == "llo", "Buffer consume mismatch");

        buffer.RetrieveAll();
        Require(buffer.ReadableBytes() == 0, "Buffer retrieve all should clear readable bytes");
    }

    void TestAddress() {
        LikesProgram::Net::Address address("127.0.0.1", 0);

        Require(address.IsValid(), "IPv4 loopback address should be valid");
        Require(address.Ip() == "127.0.0.1", "Address IP mismatch");
        Require(address.Port() == 0, "Address port mismatch");
        Require(!address.ToString().empty(), "Address ToString should not be empty");
    }

    void TestUserSecureExtensionPoint() {
        UserSecureTcpTransport tcpTransport(LikesProgram::Net::kInvalidSocket);
        UserSecureUdpTransport udpTransport(LikesProgram::Net::kInvalidSocket);
        const std::uint8_t data[] = { 'o', 'k' };

        Require(tcpTransport.Kind() == LikesProgram::Net::TransportKind::Tcp,
            "UserSecureTcpTransport kind mismatch");
        Require(udpTransport.Kind() == LikesProgram::Net::TransportKind::Udp,
            "UserSecureUdpTransport kind mismatch");
        Require(tcpTransport.InitializeSecureLayer().status == LikesProgram::Net::IoStatus::Ok,
            "UserSecureTcpTransport secure init mismatch");
        Require(tcpTransport.Initialized(), "UserSecureTcpTransport init flag mismatch");
        Require(tcpTransport.SecurityState() == LikesProgram::Net::TransportSecurityState::Plain,
            "InitializeSecureLayer should not enter handshake state");
        Require(tcpTransport.WriteSome(data, sizeof(data)).nbytes == 2,
            "UserSecureTcpTransport plain write mismatch");
        Require(tcpTransport.SocketWriteUsed(), "UserSecureTcpTransport should keep socket write by default");
        Require(!tcpTransport.SecureWriteUsed(), "UserSecureTcpTransport should not use secure write before upgrade");

        Require(udpTransport.UpgradeCommunication().status == LikesProgram::Net::IoStatus::Ok,
            "UserSecureUdpTransport upgrade result mismatch");
        Require(udpTransport.Upgraded(), "UserSecureUdpTransport upgrade flag mismatch");
        Require(udpTransport.NeedHandshake(), "UserSecureUdpTransport should enter handshake state after upgrade");
        Require(udpTransport.Handshake().status == LikesProgram::Net::IoStatus::Ok,
            "UserSecureUdpTransport handshake result mismatch");
        Require(udpTransport.HandshakeCompleted(), "UserSecureUdpTransport handshake callback mismatch");
        Require(udpTransport.SecurityState() == LikesProgram::Net::TransportSecurityState::Secure,
            "UserSecureUdpTransport secure state mismatch");
        Require(udpTransport.WriteSome(data, sizeof(data)).nbytes == 2,
            "UserSecureUdpTransport secure write mismatch");
        Require(udpTransport.SecureWriteUsed(), "UserSecureUdpTransport should use secure write after handshake");
    }

    void TestDelayedUpgradeStateMachine() {
        UserSecureTcpTransport transport(LikesProgram::Net::kInvalidSocket);
        const std::uint8_t data[] = { 'p', 'l', 'a', 'i', 'n' };

        Require(transport.InitializeSecureLayer().status == LikesProgram::Net::IoStatus::Ok,
            "Delayed upgrade init should succeed");
        Require(transport.SecurityState() == LikesProgram::Net::TransportSecurityState::Plain,
            "Delayed upgrade should start in plain state");
        Require(transport.WriteSome(data, sizeof(data)).status == LikesProgram::Net::IoStatus::Ok,
            "Delayed upgrade plain write should use socket fallback");
        Require(transport.SocketWriteUsed(), "Delayed upgrade should keep socket ability before upgrade");

        Require(transport.UpgradeCommunication().status == LikesProgram::Net::IoStatus::Ok,
            "Delayed upgrade request should succeed");
        Require(transport.NeedHandshake(), "Delayed upgrade should enter handshake state");
        Require(transport.Handshake().status == LikesProgram::Net::IoStatus::Ok,
            "Delayed upgrade handshake should succeed");
        Require(transport.SecurityState() == LikesProgram::Net::TransportSecurityState::Secure,
            "Delayed upgrade should enter secure state after handshake");
        Require(transport.WriteSome(data, sizeof(data)).status == LikesProgram::Net::IoStatus::Ok,
            "Delayed upgrade secure write should succeed");
        Require(transport.SecureWriteUsed(), "Delayed upgrade should use secure hook after handshake");
    }

    void TestRoundTrip(LikesProgram::Net::TransportKind kind, const char* label) {
        LikesProgram::Net::Server server(
            LikesProgram::Net::Address("127.0.0.1", 0),
            kind,
            [kind](LikesProgram::Net::SocketType fd, LikesProgram::Net::EventLoop* loop) {
                return std::make_shared<EchoConnection>(fd, loop, MakePlainTransport(fd, kind));
            });

        server.Start();
        const auto listenAddresses = server.GetListenAddresses();
        Require(!listenAddresses.empty(), "Server should expose bound address");
        Require(listenAddresses.front().Port() != 0, "Server should expose assigned port");

        std::atomic<bool> received{ false };
        std::string payload;

        LikesProgram::Net::Client client(
            LikesProgram::Net::Address("127.0.0.1", listenAddresses.front().Port()),
            kind,
            [&received, &payload, kind](
                LikesProgram::Net::SocketType fd,
                LikesProgram::Net::EventLoop* loop) {
                return std::make_shared<CaptureConnection>(
                    fd,
                    loop,
                    received,
                    payload,
                    MakePlainTransport(fd, kind));
            });

        client.Start();
        auto connection = client.GetConnection();
        Require(connection != nullptr, "Client should create connection");
        connection->Send("ping", 4);

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!received.load(std::memory_order_acquire)
            && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        client.Shutdown();
        server.Shutdown();

        Require(received.load(std::memory_order_acquire), label);
        Require(payload == "ping", "echo payload mismatch");
    }

    void TestImmediateSecureUpgradeKeepsSocketFallback() {
        auto sharedContext = std::make_shared<StartTlsLikeTcpTransport::SharedSecureContext>();
        std::atomic<int> serverHandshakes{ 0 };

        LikesProgram::Net::Server server(
            LikesProgram::Net::Address("127.0.0.1", 0),
            LikesProgram::Net::TransportKind::Tcp,
            LikesProgram::Net::ConnectionFactory(
                [sharedContext, &serverHandshakes](
                    LikesProgram::Net::SocketType fd,
                    LikesProgram::Net::EventLoop* loop) {
                    auto transport = std::make_unique<StartTlsLikeTcpTransport>(fd, sharedContext);
                    return std::make_shared<ImmediateUpgradeConnection>(
                        fd,
                        loop,
                        std::move(transport),
                        serverHandshakes);
                },
                [sharedContext]() {
                    sharedContext->loadCount.fetch_add(1, std::memory_order_acq_rel);
                    return true;
                }));

        server.Start();
        const auto listenAddresses = server.GetListenAddresses();
        Require(!listenAddresses.empty(), "Immediate secure server should expose bound address");

        std::atomic<bool> received{ false };
        std::string payload;

        LikesProgram::Net::Client client(
            LikesProgram::Net::Address("127.0.0.1", listenAddresses.front().Port()),
            LikesProgram::Net::TransportKind::Tcp,
            [&received, &payload](
                LikesProgram::Net::SocketType fd,
                LikesProgram::Net::EventLoop* loop) {
                return std::make_shared<CaptureConnection>(
                    fd,
                    loop,
                    received,
                    payload,
                    std::make_unique<LikesProgram::Net::TcpTransport>(fd));
            });

        client.Start();
        auto connection = client.GetConnection();
        Require(connection != nullptr, "Immediate secure client should create connection");
        connection->Send("secure", 6);

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!received.load(std::memory_order_acquire)
            && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        client.Shutdown();
        server.Shutdown();

        Require(received.load(std::memory_order_acquire), "Immediate secure echo should complete");
        Require(payload == "secure", "Immediate secure echo payload mismatch");
        Require(serverHandshakes.load(std::memory_order_acquire) > 0,
            "OnSecureLayerReady should trigger handshake before message handling");
        Require(sharedContext->loadCount.load(std::memory_order_acquire) == 1,
            "Shared secure context should not be loaded once per connection");
    }

    void TestSharedSecureResourcesInitializeOnceBeforeConnections() {
        auto state = std::make_shared<SharedSecureFactoryState>();

        LikesProgram::Net::Server server(
            LikesProgram::Net::Address("127.0.0.1", 0),
            LikesProgram::Net::TransportKind::Tcp,
            LikesProgram::Net::ConnectionFactory(
                [state](
                    LikesProgram::Net::SocketType fd,
                    LikesProgram::Net::EventLoop* loop) {
                    Require(state->sharedInitCount.load(std::memory_order_acquire) == 1,
                        "Shared secure resources should initialize before connection creation");
                    state->connectionCreateCount.fetch_add(1, std::memory_order_acq_rel);
                    return std::make_shared<EchoConnection>(
                        fd,
                        loop,
                        std::make_unique<LikesProgram::Net::TcpTransport>(fd));
                },
                [state]() {
                    state->sharedInitCount.fetch_add(1, std::memory_order_acq_rel);
                    return true;
                }));

        server.Start();
        const auto listenAddresses = server.GetListenAddresses();
        Require(!listenAddresses.empty(), "Shared secure server should expose bound address");

        std::atomic<bool> received{ false };
        std::string payload;

        LikesProgram::Net::Client client(
            LikesProgram::Net::Address("127.0.0.1", listenAddresses.front().Port()),
            LikesProgram::Net::TransportKind::Tcp,
            [&received, &payload](
                LikesProgram::Net::SocketType fd,
                LikesProgram::Net::EventLoop* loop) {
                return std::make_shared<CaptureConnection>(
                    fd,
                    loop,
                    received,
                    payload,
                    std::make_unique<LikesProgram::Net::TcpTransport>(fd));
            });

        client.Start();
        auto connection = client.GetConnection();
        Require(connection != nullptr, "Shared secure client should create connection");
        connection->Send("shared", 6);

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!received.load(std::memory_order_acquire)
            && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        client.Shutdown();
        server.Shutdown();

        Require(received.load(std::memory_order_acquire), "Shared secure echo should complete");
        Require(payload == "shared", "Shared secure echo payload mismatch");
        Require(state->sharedInitCount.load(std::memory_order_acquire) == 1,
            "Shared secure resources should initialize exactly once");
        Require(state->connectionCreateCount.load(std::memory_order_acquire) == 1,
            "Server should create one accepted connection");
    }

    void TestSharedSecureResourceFailureStopsClient() {
        auto state = std::make_shared<SharedSecureFactoryState>();

        LikesProgram::Net::Client client(
            LikesProgram::Net::Address("127.0.0.1", 9),
            LikesProgram::Net::TransportKind::Tcp,
            LikesProgram::Net::ConnectionFactory(
                [state](
                    LikesProgram::Net::SocketType fd,
                    LikesProgram::Net::EventLoop* loop) {
                    (void)fd;
                    (void)loop;
                    state->connectionCreateCount.fetch_add(1, std::memory_order_acq_rel);
                    return std::shared_ptr<LikesProgram::Net::Connection>{};
                },
                [state]() {
                    state->sharedInitCount.fetch_add(1, std::memory_order_acq_rel);
                    return false;
                }));

        client.Start();

        Require(client.GetStatus() == LikesProgram::Net::Client::Status::Stopped,
            "Client should stop when shared secure resources fail");
        Require(client.GetConnection() == nullptr,
            "Client should not create connection after shared secure failure");
        Require(state->sharedInitCount.load(std::memory_order_acquire) == 1,
            "Shared secure failure initializer should run once");
        Require(state->connectionCreateCount.load(std::memory_order_acquire) == 0,
            "Connection factory should not run after shared secure failure");
    }
}

int main() {
    try {
        TestPackageIdentity();
        TestBuffer();
        TestAddress();
        TestUserSecureExtensionPoint();
        TestDelayedUpgradeStateMachine();
        TestImmediateSecureUpgradeKeepsSocketFallback();
        TestSharedSecureResourcesInitializeOnceBeforeConnections();
        TestSharedSecureResourceFailureStopsClient();
        TestRoundTrip(LikesProgram::Net::TransportKind::Tcp, "Client should receive TCP echo");
        TestRoundTrip(LikesProgram::Net::TransportKind::Udp, "Client should receive UDP echo");
        std::cout << "LikesProgramNetTests passed\n";
        return 0;
    }
    catch (const std::exception& ex) {
        std::cerr << "LikesProgramNetTests failed: " << ex.what() << '\n';
        return 1;
    }
}
