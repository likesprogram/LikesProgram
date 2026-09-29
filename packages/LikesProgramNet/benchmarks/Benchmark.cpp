#include <LikesProgram/Net/Net.hpp>
#include "net/UdpTransport.hpp"
#include "ProxyRelayBenchmark.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/socket.h>
#endif

namespace {
    using Clock = std::chrono::steady_clock;

    struct Measurement {
        const char* name = "";      // 输出项名称，保持脚本友好的稳定文本。
        const char* unit = "";      // 吞吐单位，供趋势采集脚本直接展示。
        double seconds = 0.0;        // 本轮耗时，单位秒。
        double throughput = 0.0;     // 本轮吞吐值，单位由 unit 说明。
    };

    struct LatencySummary {
        const char* name = "";      // 延迟输出项名称。
        std::size_t samples = 0;     // 参与统计的样本数量。
        double p50 = 0.0;            // 50 分位延迟，单位微秒。
        double p95 = 0.0;            // 95 分位延迟，单位微秒。
        double p99 = 0.0;            // 99 分位延迟，单位微秒。
        double max = 0.0;            // 最大延迟，单位微秒。
    };

    struct SlowReaderSummary {
        double seconds = 0.0;                  // 从开始发送到服务端触发背压的耗时。
        std::int64_t submittedBytes = 0;       // 客户端在背压观察前提交的总字节数。
        std::size_t maximumPendingBytes = 0;   // 服务端高水位回调观察到的最大队列字节数。
        int highWatermarkEvents = 0;           // 服务端高水位回调次数。
        int overflowEvents = 0;                // 服务端硬上限回调次数，稳定路径应为零。
    };

    struct EngineContractSummary {
        std::size_t tlsCiphertextBytes = 0;    // TLS 各阶段产生的密文总字节数。
        std::size_t tlsPlaintextBytes = 0;     // TLS 解密阶段交付的明文总字节数。
        std::size_t dtlsDatagrams = 0;         // DTLS 各阶段产生或交付的数据报数量。
        std::size_t dtlsPayloadBytes = 0;      // DTLS 各阶段数据报 payload 总字节数。
    };

    struct SlowReaderState {
        std::atomic<int> highWatermarkEvents{ 0 }; // 服务端首次进入背压区间的次数
        std::atomic<int> overflowEvents{ 0 }; // 服务端写队列超过硬上限的次数
        std::atomic<std::size_t> maximumPendingBytes{ 0 }; // 高水位观察到的最大队列
    };

    const char* PlatformName() noexcept {
#ifdef _WIN32
        return "windows";
#elif defined(__APPLE__)
        return "apple";
#elif defined(__linux__)
        return "linux";
#else
        return "posix";
#endif
    }

    bool SetBenchNonBlocking(LikesProgram::Net::SocketType fd) noexcept {
#ifdef _WIN32
        u_long mode = 1UL; // Windows 使用 ioctlsocket 切换非阻塞。
        return ::ioctlsocket(fd, FIONBIO, &mode) == 0;
#else
        const int flags = ::fcntl(fd, F_GETFL, 0); // POSIX 保留原 fd 标志后追加 O_NONBLOCK。
        return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
    }

    double SafeElapsedSeconds(const Clock::time_point& started) noexcept {
        const double elapsed = std::chrono::duration<double>(Clock::now() - started).count(); // 单调钟测量本轮耗时。
        return elapsed > 0.0 ? elapsed : 1e-12;
    }

    template <typename Predicate>
    bool WaitUntil(Predicate predicate, std::chrono::milliseconds timeout) {
        const auto deadline = Clock::now() + timeout; // 每个 benchmark 子项都有明确超时边界。
        while (!predicate() && Clock::now() < deadline) {
            std::this_thread::yield(); // raw benchmark 不人为加入 1ms 采样间隔，避免把发压器睡眠计入吞吐。
        }
        return predicate();
    }

    std::shared_ptr<LikesProgram::Net::Connection> WaitForClientConnection(
        LikesProgram::Net::Client& client) {
        std::shared_ptr<LikesProgram::Net::Connection> connection; // 保存异步 connect 完成后的连接快照。
        const bool connected = WaitUntil(
            [&client, &connection]() {
                connection = client.GetConnection(); // IOCP connect 通过 completion 异步发布连接。
                return static_cast<bool>(connection);
            },
            std::chrono::seconds(2));
        return connected ? connection : nullptr;
    }

    class EchoConnection final : public LikesProgram::Net::Connection {
    public:
        // benchmark 按协议创建连接，TCP 直接 completion-owned，UDP 使用包内兼容路径。
        EchoConnection(
            LikesProgram::Net::SocketType fd,
            LikesProgram::Net::EventLoop* loop,
            LikesProgram::Net::TransportKind kind)
            : Connection(fd, loop, kind) {
        }

    protected:
        void OnMessage(LikesProgram::Net::Buffer& in) override {
            // raw foundation 口径：业务层只做回写和消费，不做协议解析。
            Send(in);
            in.RetrieveAll();
        }
    };

    // 只验证公开 TLS Engine 边界，不模拟具体密码实现或密码性能。
    class BenchmarkTlsEngine final : public LikesProgram::Net::TlsEngine {
    public:
        // 产生固定握手密文，作为应用侧 Engine 的最小输出样本。
        LikesProgram::Net::TlsResult StartHandshake(
            LikesProgram::Net::BufferChain& ciphertextOutput) override {
            LikesProgram::Net::Buffer flight(0); // 固定握手 flight，不依赖外部密码库
            flight.Append("tls-flight", sizeof("tls-flight") - 1);
            ciphertextOutput.Append(std::move(flight));
            return { LikesProgram::Net::TlsAction::NeedCiphertext, 0 };
        }

        // 消费完整密文链并交付一段明文，验证 ownership 在调用边界内闭合。
        LikesProgram::Net::TlsResult ConsumeCiphertext(
            LikesProgram::Net::BufferChain& ciphertextInput,
            LikesProgram::Net::BufferChain& plaintextOutput,
            LikesProgram::Net::BufferChain&) override {
            ciphertextInput.Consume(ciphertextInput.ReadableBytes());
            LikesProgram::Net::Buffer plaintext(0); // 固定解密结果，仅用于契约计数
            plaintext.Append("tls-plain", sizeof("tls-plain") - 1);
            plaintextOutput.Append(std::move(plaintext));
            m_state = LikesProgram::Net::TlsState::Active;
            return { LikesProgram::Net::TlsAction::PlaintextReady, 0 };
        }

        // 消费应用明文并产生固定密文，验证反向 BufferChain 数据链。
        LikesProgram::Net::TlsResult ConsumePlaintext(
            LikesProgram::Net::BufferChain& plaintextInput,
            LikesProgram::Net::BufferChain& ciphertextOutput) override {
            plaintextInput.Consume(plaintextInput.ReadableBytes());
            LikesProgram::Net::Buffer ciphertext(0); // 固定加密结果，不代表密码吞吐
            ciphertext.Append("tls-cipher", sizeof("tls-cipher") - 1);
            ciphertextOutput.Append(std::move(ciphertext));
            return { LikesProgram::Net::TlsAction::CiphertextReady, 0 };
        }

        // 产生固定关闭密文并完成 Engine 状态收敛。
        LikesProgram::Net::TlsResult Shutdown(
            LikesProgram::Net::BufferChain& ciphertextOutput) override {
            LikesProgram::Net::Buffer closeFlight(0); // close_notify 等价的契约样本
            closeFlight.Append("tls-close", sizeof("tls-close") - 1);
            ciphertextOutput.Append(std::move(closeFlight));
            m_state = LikesProgram::Net::TlsState::Closed;
            return { LikesProgram::Net::TlsAction::CloseTransport, 0 };
        }

        // 返回当前 TLS 契约状态。
        LikesProgram::Net::TlsState State() const noexcept override {
            return m_state;
        }

        // 返回稳定的诊断协议名，不依赖 ALPN 库。
        const char* NegotiatedProtocol() const noexcept override {
            return "benchmark-tls-contract";
        }

    private:
        LikesProgram::Net::TlsState m_state = LikesProgram::Net::TlsState::Handshaking; // 契约会话状态
    };

    // 只验证公开 DTLS Engine 边界，不模拟丢包、重传或密码性能。
    class BenchmarkDtlsEngine final : public LikesProgram::Net::DtlsEngine {
    public:
        // 产生首个完整密文数据报，并请求框架安排重传 timer。
        LikesProgram::Net::DtlsResult StartHandshake(
            LikesProgram::Net::DtlsDatagramBatch& ciphertextOutput) override {
            LikesProgram::Net::Buffer flight(0); // 独立数据报，保留 datagram 边界
            flight.Append("dtls-flight", sizeof("dtls-flight") - 1);
            ciphertextOutput.Append(std::move(flight));
            return {
                LikesProgram::Net::DtlsAction::CiphertextReady
                    | LikesProgram::Net::DtlsAction::ArmRetransmitTimer,
                0,
                25
            };
        }

        // 消费一个密文数据报并交付一个明文数据报。
        LikesProgram::Net::DtlsResult ConsumeCiphertext(
            LikesProgram::Net::Buffer& ciphertextDatagram,
            LikesProgram::Net::DtlsDatagramBatch& plaintextOutput,
            LikesProgram::Net::DtlsDatagramBatch&) override {
            ciphertextDatagram.RetrieveAll();
            LikesProgram::Net::Buffer plaintext(0); // 保留一进一出数据报契约
            plaintext.Append("dtls-plain", sizeof("dtls-plain") - 1);
            plaintextOutput.Append(std::move(plaintext));
            m_state = LikesProgram::Net::DtlsState::Active;
            return {
                LikesProgram::Net::DtlsAction::PlaintextReady
                    | LikesProgram::Net::DtlsAction::CancelRetransmitTimer,
                0,
                0
            };
        }

        // 消费一个明文数据报并产生一个密文数据报。
        LikesProgram::Net::DtlsResult ConsumePlaintext(
            LikesProgram::Net::Buffer& plaintextDatagram,
            LikesProgram::Net::DtlsDatagramBatch& ciphertextOutput) override {
            plaintextDatagram.RetrieveAll();
            LikesProgram::Net::Buffer ciphertext(0); // 应用侧 Engine 的最小出站样本
            ciphertext.Append("dtls-cipher", sizeof("dtls-cipher") - 1);
            ciphertextOutput.Append(std::move(ciphertext));
            return { LikesProgram::Net::DtlsAction::CiphertextReady, 0, 0 };
        }

        // 产生一次重传 flight，验证 timer action 与数据报输出可组合。
        LikesProgram::Net::DtlsResult HandleTimeout(
            LikesProgram::Net::DtlsDatagramBatch& ciphertextOutput) override {
            LikesProgram::Net::Buffer retryFlight(0); // 固定重传数据报
            retryFlight.Append("dtls-retry", sizeof("dtls-retry") - 1);
            ciphertextOutput.Append(std::move(retryFlight));
            return {
                LikesProgram::Net::DtlsAction::CiphertextReady
                    | LikesProgram::Net::DtlsAction::ArmRetransmitTimer,
                0,
                25
            };
        }

        // 产生关闭数据报并完成 peer session 收敛。
        LikesProgram::Net::DtlsResult Shutdown(
            LikesProgram::Net::DtlsDatagramBatch& ciphertextOutput) override {
            LikesProgram::Net::Buffer closeDatagram(0); // close alert 等价的契约样本
            closeDatagram.Append("dtls-close", sizeof("dtls-close") - 1);
            ciphertextOutput.Append(std::move(closeDatagram));
            m_state = LikesProgram::Net::DtlsState::Closed;
            return { LikesProgram::Net::DtlsAction::CloseSession, 0, 0 };
        }

        // 返回当前 DTLS peer 状态。
        LikesProgram::Net::DtlsState State() const noexcept override {
            return m_state;
        }

        // 返回稳定的诊断协议名，不依赖具体 DTLS 库。
        const char* NegotiatedProtocol() const noexcept override {
            return "benchmark-dtls-contract";
        }

    private:
        LikesProgram::Net::DtlsState m_state = LikesProgram::Net::DtlsState::Handshaking; // peer 会话状态
    };

    class CountingConnection final : public LikesProgram::Net::Connection {
    public:
        // benchmark 按协议创建计数连接，不接收 Transport 实现对象。
        CountingConnection(
            LikesProgram::Net::SocketType fd,
            LikesProgram::Net::EventLoop* loop,
            std::atomic<std::int64_t>& receivedBytes,
            LikesProgram::Net::TransportKind kind)
            : Connection(fd, loop, kind),
              m_receivedBytes(receivedBytes) {
        }

    protected:
        void OnMessage(LikesProgram::Net::Buffer& in) override {
            // 按字节计数可以容忍 TCP 合包/拆包，不把帧边界当作 benchmark 前提。
            m_receivedBytes.fetch_add(
                static_cast<std::int64_t>(in.ReadableBytes()),
                std::memory_order_release);
            in.RetrieveAll();
        }

    private:
        std::atomic<std::int64_t>& m_receivedBytes; // benchmark 主线程等待的累计回显字节数。
    };

    class FullDuplexConnection final : public LikesProgram::Net::Connection {
    public:
        // 连接建立后立即发送本端 payload，并统计同时收到的对端字节。
        FullDuplexConnection(
            LikesProgram::Net::SocketType fd,
            LikesProgram::Net::EventLoop* loop,
            std::atomic<std::int64_t>& receivedBytes,
            std::size_t payloadBytes)
            : Connection(fd, loop),
              m_receivedBytes(receivedBytes),
              m_payload(payloadBytes, 'd') {
        }

    protected:
        void OnConnected() override {
            // 双端都在 OnConnected 投递写链，稳定形成同时读写窗口。
            Send(m_payload.data(), m_payload.size());
        }

        void OnMessage(LikesProgram::Net::Buffer& in) override {
            m_receivedBytes.fetch_add(
                static_cast<std::int64_t>(in.ReadableBytes()),
                std::memory_order_release);
            in.RetrieveAll();
        }

    private:
        std::atomic<std::int64_t>& m_receivedBytes; // 本端累计收到的对端字节数
        std::string m_payload; // OnConnected 一次性提交的固定 payload
    };

    class SlowReaderEchoConnection final : public LikesProgram::Net::Connection {
    public:
        // 固定较低水位，稳定制造慢客户端下的写队列背压。
        SlowReaderEchoConnection(
            LikesProgram::Net::SocketType fd,
            LikesProgram::Net::EventLoop* loop,
            SlowReaderState& state)
            : Connection(fd, loop),
              m_state(state) {
            SetWriteWatermark(64 * 1024, 16 * 1024);
            SetMaxPendingWriteBytes(4 * 1024 * 1024);
        }

    protected:
        void OnMessage(LikesProgram::Net::Buffer& in) override {
            // 回显负载持续进入 completion 写链，直到高水位暂停读端。
            Send(in);
            in.RetrieveAll();
        }

        void OnWriteHighWatermark(std::size_t pendingBytes) override {
            std::size_t observed = m_state.maximumPendingBytes.load(std::memory_order_relaxed);
            while (observed < pendingBytes
                && !m_state.maximumPendingBytes.compare_exchange_weak(
                    observed,
                    pendingBytes,
                    std::memory_order_release,
                    std::memory_order_relaxed)) {
            }
            m_state.highWatermarkEvents.fetch_add(1, std::memory_order_release);
            PauseReading(); // 慢下游出现后立即向上游施加背压
        }

        void OnWriteLowWatermark(std::size_t) override {
            ResumeReading(); // 下游恢复后允许继续接收业务数据
        }

        void OnWriteQueueOverflow(std::size_t) override {
            m_state.overflowEvents.fetch_add(1, std::memory_order_release);
        }

    private:
        SlowReaderState& m_state; // benchmark 主线程读取的服务端背压状态
    };

    std::unique_ptr<LikesProgram::Net::Server> StartEchoServer(
        LikesProgram::Net::TransportKind kind,
        std::size_t workerThreads) {
        auto server = std::make_unique<LikesProgram::Net::Server>(
            LikesProgram::Net::Address("127.0.0.1", 0),
            kind,
            [kind](LikesProgram::Net::SocketType fd, LikesProgram::Net::EventLoop* loop) {
                // 服务端连接只承担回显，避免把上层协议成本混入 raw benchmark。
                return std::make_shared<EchoConnection>(fd, loop, kind);
            });

        server->SetWorkerThreads(workerThreads);
        server->Start();
        return server;
    }

    std::uint16_t FirstListenPort(const LikesProgram::Net::Server& server) {
        const auto listenAddresses = server.GetListenAddresses(); // 端口为 0 时 Start 后由系统分配。
        if (listenAddresses.empty() || listenAddresses.front().Port() == 0) {
            throw std::runtime_error("Net benchmark server did not bind");
        }
        return listenAddresses.front().Port();
    }

    Measurement MeasureTcpEchoBytes(std::int64_t bytes) {
        auto server = StartEchoServer(LikesProgram::Net::TransportKind::Tcp, 2);
        const std::uint16_t port = FirstListenPort(*server); // 客户端连接本地回环分配端口。

        std::atomic<std::int64_t> receivedBytes{ 0 }; // 客户端已收到的 echo 字节数。
        LikesProgram::Net::Client client(
            LikesProgram::Net::Address("127.0.0.1", port),
            LikesProgram::Net::TransportKind::Tcp,
            [&receivedBytes](LikesProgram::Net::SocketType fd, LikesProgram::Net::EventLoop* loop) {
                return std::make_shared<CountingConnection>(
                    fd,
                    loop,
                    receivedBytes,
                    LikesProgram::Net::TransportKind::Tcp);
            });

        client.Start();
        auto connection = WaitForClientConnection(client); // 等待 completion connect 完成后再发送。
        if (!connection) throw std::runtime_error("Net benchmark TCP client did not connect");

        std::string payload(static_cast<std::size_t>(bytes), 'x'); // 固定 payload，避免协议解析成本混入。
        const auto started = Clock::now();
        connection->Send(payload.data(), payload.size());

        const bool completed = WaitUntil(
            [&receivedBytes, bytes]() {
                return receivedBytes.load(std::memory_order_acquire) >= bytes;
            },
            std::chrono::seconds(5));

        const double elapsed = SafeElapsedSeconds(started);
        client.Shutdown();
        server->Shutdown();

        if (!completed) throw std::runtime_error("Net benchmark TCP echo timed out");
        return Measurement{ "tcp_echo_bytes_per_second", "bytes/s", elapsed, static_cast<double>(bytes) / elapsed };
    }

    Measurement MeasureTcpMultiClientBytes(std::size_t clientCount, std::int64_t bytesPerClient) {
        auto server = StartEchoServer(LikesProgram::Net::TransportKind::Tcp, 2);
        const std::uint16_t port = FirstListenPort(*server); // 所有客户端共享同一服务端监听口。

        std::atomic<std::int64_t> receivedBytes{ 0 }; // 多客户端累计回显字节数。
        std::vector<std::unique_ptr<LikesProgram::Net::Client>> clients; // 客户端生命周期由 benchmark 控制。
        std::vector<std::shared_ptr<LikesProgram::Net::Connection>> connections; // 保留连接快照便于统一发送。
        clients.reserve(clientCount);
        connections.reserve(clientCount);

        for (std::size_t i = 0; i < clientCount; ++i) {
            auto client = std::make_unique<LikesProgram::Net::Client>(
                LikesProgram::Net::Address("127.0.0.1", port),
                LikesProgram::Net::TransportKind::Tcp,
                [&receivedBytes](LikesProgram::Net::SocketType fd, LikesProgram::Net::EventLoop* loop) {
                    return std::make_shared<CountingConnection>(
                        fd,
                        loop,
                        receivedBytes,
                        LikesProgram::Net::TransportKind::Tcp);
                });
            client->Start();

            auto connection = WaitForClientConnection(*client); // 每个客户端都等待独立 completion connect。
            if (!connection) throw std::runtime_error("Net benchmark multi client did not connect");
            connections.push_back(connection);
            clients.push_back(std::move(client));
        }

        const std::int64_t totalBytes = bytesPerClient * static_cast<std::int64_t>(clientCount); // 多连接总发送量。
        std::string payload(static_cast<std::size_t>(bytesPerClient), 'm'); // 每个连接发送同等 payload。
        const auto started = Clock::now();
        for (const auto& connection : connections) {
            connection->Send(payload.data(), payload.size());
        }

        const bool completed = WaitUntil(
            [&receivedBytes, totalBytes]() {
                return receivedBytes.load(std::memory_order_acquire) >= totalBytes;
            },
            std::chrono::seconds(5));

        const double elapsed = SafeElapsedSeconds(started);
        for (auto& client : clients) client->Shutdown();
        server->Shutdown();

        if (!completed) throw std::runtime_error("Net benchmark TCP multi client timed out");
        return Measurement{
            "tcp_multi_client_bytes_per_second",
            "bytes/s",
            elapsed,
            static_cast<double>(totalBytes) / elapsed
        };
    }

    Measurement MeasureTcpIdleConnections(std::size_t connectionCount) {
        auto server = StartEchoServer(LikesProgram::Net::TransportKind::Tcp, 2);
        const std::uint16_t port = FirstListenPort(*server); // 空闲连接只验证连接生命周期成本。

        std::vector<std::unique_ptr<LikesProgram::Net::Client>> clients; // 保持连接存活直到测量结束。
        clients.reserve(connectionCount);

        const auto started = Clock::now();
        for (std::size_t i = 0; i < connectionCount; ++i) {
            auto client = std::make_unique<LikesProgram::Net::Client>(
                LikesProgram::Net::Address("127.0.0.1", port),
                LikesProgram::Net::TransportKind::Tcp,
                [](LikesProgram::Net::SocketType fd, LikesProgram::Net::EventLoop* loop) {
                    return std::make_shared<LikesProgram::Net::Connection>(fd, loop);
                });
            client->Start();
            if (!WaitForClientConnection(*client)) throw std::runtime_error("Net benchmark idle client did not connect");
            clients.push_back(std::move(client));
        }

        const double elapsed = SafeElapsedSeconds(started);
        std::this_thread::sleep_for(std::chrono::milliseconds(50)); // 短暂停留，确认连接能保持空闲状态。

        for (auto& client : clients) client->Shutdown();
        server->Shutdown();

        return Measurement{
            "tcp_idle_connections_per_second",
            "connections/s",
            elapsed,
            static_cast<double>(connectionCount) / elapsed
        };
    }

    Measurement MeasureTcpFullDuplexBytes(std::size_t bytesPerDirection) {
        std::atomic<std::int64_t> serverReceived{ 0 }; // 服务端收到的客户端 payload
        std::atomic<std::int64_t> clientReceived{ 0 }; // 客户端收到的服务端 payload
        LikesProgram::Net::Server server(
            LikesProgram::Net::Address("127.0.0.1", 0),
            [&serverReceived, bytesPerDirection](
                LikesProgram::Net::SocketType fd,
                LikesProgram::Net::EventLoop* loop) {
                return std::make_shared<FullDuplexConnection>(
                    fd,
                    loop,
                    serverReceived,
                    bytesPerDirection);
            });
        server.SetWorkerThreads(2);
        server.Start();
        const std::uint16_t port = FirstListenPort(server); // 双端共享同一回环连接

        LikesProgram::Net::Client client(
            LikesProgram::Net::Address("127.0.0.1", port),
            [&clientReceived, bytesPerDirection](
                LikesProgram::Net::SocketType fd,
                LikesProgram::Net::EventLoop* loop) {
                return std::make_shared<FullDuplexConnection>(
                    fd,
                    loop,
                    clientReceived,
                    bytesPerDirection);
            });

        const auto started = Clock::now();
        client.Start(); // Connect completion 后两端同时从 OnConnected 提交 payload
        const std::int64_t expectedBytes = static_cast<std::int64_t>(bytesPerDirection);
        const bool completed = WaitUntil(
            [&serverReceived, &clientReceived, expectedBytes]() {
                return serverReceived.load(std::memory_order_acquire) >= expectedBytes
                    && clientReceived.load(std::memory_order_acquire) >= expectedBytes;
            },
            std::chrono::seconds(5));
        const double elapsed = SafeElapsedSeconds(started);

        client.Shutdown();
        server.Shutdown();
        if (!completed) throw std::runtime_error("Net benchmark TCP full duplex timed out");
        const double totalBytes = static_cast<double>(bytesPerDirection) * 2.0;
        return Measurement{
            "tcp_full_duplex_bytes_per_second",
            "bytes/s",
            elapsed,
            totalBytes / elapsed
        };
    }

    SlowReaderSummary MeasureTcpSlowReaderBackpressure() {
        SlowReaderState state; // 服务端 worker 与 benchmark 主线程共享背压观察值
        LikesProgram::Net::Server server(
            LikesProgram::Net::Address("127.0.0.1", 0),
            [&state](LikesProgram::Net::SocketType fd, LikesProgram::Net::EventLoop* loop) {
                return std::make_shared<SlowReaderEchoConnection>(fd, loop, state);
            });
        server.SetWorkerThreads(2);
        server.Start();
        const std::uint16_t port = FirstListenPort(server); // 使用内核分配的回环端口

        LikesProgram::Net::Client client(
            LikesProgram::Net::Address("127.0.0.1", port),
            [](LikesProgram::Net::SocketType fd, LikesProgram::Net::EventLoop* loop) {
                return std::make_shared<LikesProgram::Net::Connection>(fd, loop);
            });
        client.Start();
        auto connection = WaitForClientConnection(client); // completion connect 后再暂停客户端读取
        if (!connection) throw std::runtime_error("Net benchmark slow reader did not connect");
        connection->PauseReading();
        std::this_thread::sleep_for(std::chrono::milliseconds(25)); // 等待 PauseReading 进入 issuer

        constexpr std::size_t kChunkBytes = 16 * 1024;
        constexpr std::int64_t kMaximumSubmittedBytes = 64 * 1024 * 1024;
        std::string payload(kChunkBytes, 's'); // 固定块避免每轮重新分配
        std::int64_t submittedBytes = 0; // 背压前已提交到客户端写链的字节数
        const auto started = Clock::now();
        while (submittedBytes < kMaximumSubmittedBytes
            && state.highWatermarkEvents.load(std::memory_order_acquire) == 0) {
            connection->Send(payload.data(), payload.size());
            submittedBytes += static_cast<std::int64_t>(payload.size());
        }

        const bool backpressureObserved = WaitUntil(
            [&state]() {
                return state.highWatermarkEvents.load(std::memory_order_acquire) > 0;
            },
            std::chrono::seconds(5));
        const double elapsed = SafeElapsedSeconds(started);

        connection->ForceClose();
        client.Shutdown();
        server.Shutdown();
        if (!backpressureObserved) {
            throw std::runtime_error("Net benchmark slow reader did not trigger backpressure");
        }
        if (state.overflowEvents.load(std::memory_order_acquire) != 0) {
            throw std::runtime_error("Net benchmark slow reader exceeded the hard write limit");
        }

        return SlowReaderSummary{
            elapsed,
            submittedBytes,
            state.maximumPendingBytes.load(std::memory_order_acquire),
            state.highWatermarkEvents.load(std::memory_order_acquire),
            state.overflowEvents.load(std::memory_order_acquire)
        };
    }

    Measurement MeasureUdpEchoDatagrams(std::int64_t datagrams) {
        auto server = StartEchoServer(LikesProgram::Net::TransportKind::Udp, 2);
        const std::uint16_t port = FirstListenPort(*server); // UDP 服务端绑定回环端口。

        std::atomic<std::int64_t> receivedBytes{ 0 }; // 客户端累计收到的 UDP echo 字节数。
        LikesProgram::Net::Client client(
            LikesProgram::Net::Address("127.0.0.1", port),
            LikesProgram::Net::TransportKind::Udp,
            [&receivedBytes](LikesProgram::Net::SocketType fd, LikesProgram::Net::EventLoop* loop) {
                return std::make_shared<CountingConnection>(
                    fd,
                    loop,
                    receivedBytes,
                    LikesProgram::Net::TransportKind::Udp);
            });

        client.Start();
        auto connection = WaitForClientConnection(client); // UDP completion connect 建立默认 peer 后即可发送 datagram。
        if (!connection) throw std::runtime_error("Net benchmark UDP client did not connect");

        const std::string payload = "udp-raw-foundation"; // 小 datagram 更接近事件处理口径。
        const std::int64_t expectedBytes = datagrams * static_cast<std::int64_t>(payload.size());
        const auto started = Clock::now();
        for (std::int64_t i = 0; i < datagrams; ++i) {
            connection->Send(payload.data(), payload.size());
        }

        const bool completed = WaitUntil(
            [&receivedBytes, expectedBytes]() {
                return receivedBytes.load(std::memory_order_acquire) >= expectedBytes;
            },
            std::chrono::seconds(5));

        const double elapsed = SafeElapsedSeconds(started);
        client.Shutdown();
        server->Shutdown();

        if (!completed) throw std::runtime_error("Net benchmark UDP echo timed out");
        return Measurement{ "udp_echo_datagrams_per_second", "datagrams/s", elapsed, static_cast<double>(datagrams) / elapsed };
    }

    Measurement MeasureUdpSendToDatagrams(std::int64_t datagrams) {
        LikesProgram::Net::Address any("127.0.0.1", 0); // 构造地址时确保 socket runtime 已就绪。
        LikesProgram::Net::SocketType receiverFd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (receiverFd == LikesProgram::Net::kInvalidSocket) {
            throw std::runtime_error("Net benchmark UDP receiver socket failed");
        }
        LikesProgram::Net::UdpTransport receiver(receiverFd);
        if (::bind(receiverFd, any.SockAddr(), any.Length()) != 0 || !SetBenchNonBlocking(receiverFd)) {
            throw std::runtime_error("Net benchmark UDP receiver setup failed");
        }

        LikesProgram::Net::Address receiverAddress = LikesProgram::Net::Address::GetLocalAddress(receiverFd);
        LikesProgram::Net::SocketType senderFd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (senderFd == LikesProgram::Net::kInvalidSocket) {
            throw std::runtime_error("Net benchmark UDP sender socket failed");
        }
        LikesProgram::Net::UdpTransport sender(senderFd);
        if (!SetBenchNonBlocking(senderFd)) {
            throw std::runtime_error("Net benchmark UDP sender nonblocking failed");
        }

        const std::uint8_t payload[] = { 's', 'e', 'n', 'd', 't', 'o' };
        const auto started = Clock::now();
        std::int64_t sentCount = 0; // 已提交的 datagram 数，和接收进度交错推进。
        std::int64_t received = 0; // 已收回的 datagram 数。
        const bool completed = WaitUntil(
            [&sender, &receiver, &receiverAddress, payload, datagrams, &sentCount, &received]() {
                for (int burst = 0; burst < 16 && sentCount < datagrams; ++burst) {
                    const auto sent = sender.SendTo(receiverAddress, payload, sizeof(payload));
                    if (sent.status == LikesProgram::Net::IoStatus::WouldBlock) return false;
                    if (sent.status != LikesProgram::Net::IoStatus::Ok) {
                        throw std::runtime_error("Net benchmark UDP SendTo failed");
                    }
                    ++sentCount;
                }

                LikesProgram::Net::Buffer buffer;
                LikesProgram::Net::Address peer;
                LikesProgram::Net::UdpReceiveDatagram item;
                item.buffer = &buffer;
                item.peer = &peer;

                while (received < sentCount) {
                    const auto read = receiver.ReadBatch(&item, 1);
                    if (read.status == LikesProgram::Net::IoStatus::WouldBlock) return false;
                    if (read.status != LikesProgram::Net::IoStatus::Ok) {
                        throw std::runtime_error("Net benchmark UDP SendTo receive failed");
                    }
                    if (item.result.status == LikesProgram::Net::IoStatus::Ok && peer.IsValid()) ++received;
                    buffer.RetrieveAll();
                }
                return received >= datagrams;
            },
            std::chrono::seconds(5));

        const double elapsed = SafeElapsedSeconds(started);
        if (!completed) throw std::runtime_error("Net benchmark UDP SendTo timed out");
        return Measurement{ "udp_sendto_datagrams_per_second", "datagrams/s", elapsed, static_cast<double>(datagrams) / elapsed };
    }

    Measurement MeasureUdpBatchDatagrams(std::int64_t datagrams, std::size_t batchSize) {
        LikesProgram::Net::Address any("127.0.0.1", 0); // 构造地址时确保 socket runtime 已就绪。
        LikesProgram::Net::SocketType receiverFd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (receiverFd == LikesProgram::Net::kInvalidSocket) {
            throw std::runtime_error("Net benchmark UDP receiver socket failed");
        }
        LikesProgram::Net::UdpTransport receiver(receiverFd);
        if (::bind(receiverFd, any.SockAddr(), any.Length()) != 0 || !SetBenchNonBlocking(receiverFd)) {
            throw std::runtime_error("Net benchmark UDP receiver setup failed");
        }

        LikesProgram::Net::Address receiverAddress = LikesProgram::Net::Address::GetLocalAddress(receiverFd);
        LikesProgram::Net::SocketType senderFd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (senderFd == LikesProgram::Net::kInvalidSocket) {
            throw std::runtime_error("Net benchmark UDP sender socket failed");
        }
        LikesProgram::Net::UdpTransport sender(senderFd);
        if (!SetBenchNonBlocking(senderFd)) {
            throw std::runtime_error("Net benchmark UDP sender nonblocking failed");
        }

        const std::uint8_t payload[] = { 'n', 'e', 't' };
        std::vector<LikesProgram::Net::UdpSendDatagram> sends(batchSize); // 批量发送描述符复用，减少测量噪声。
        for (auto& item : sends) {
            item.peer = &receiverAddress;
            item.data = payload;
            item.len = sizeof(payload);
        }

        std::int64_t completed = 0; // 已完成收发闭环的 datagram 数。
        const auto started = Clock::now();
        while (completed < datagrams) {
            const std::size_t currentBatch =
                static_cast<std::size_t>(std::min<std::int64_t>(
                    static_cast<std::int64_t>(batchSize),
                    datagrams - completed));
            const auto sent = sender.SendBatch(sends.data(), currentBatch);
            if (sent.status != LikesProgram::Net::IoStatus::Ok) {
                throw std::runtime_error("Net benchmark UDP batch send failed");
            }

            std::int64_t received = 0; // 当前 batch 已收回的 datagram 数。
            while (received < static_cast<std::int64_t>(currentBatch)) {
                std::vector<LikesProgram::Net::Buffer> buffers;
                std::vector<LikesProgram::Net::UdpReceiveDatagram> receives;
                buffers.reserve(currentBatch);
                receives.resize(currentBatch);
                for (std::size_t i = 0; i < currentBatch; ++i) {
                    buffers.emplace_back(LikesProgram::Net::Buffer::kInitialSize);
                    receives[i].buffer = &buffers.back();
                }

                const auto read = receiver.ReadBatch(receives.data(), currentBatch);
                if (read.status == LikesProgram::Net::IoStatus::WouldBlock) {
                    std::this_thread::yield();
                    continue;
                }
                if (read.status != LikesProgram::Net::IoStatus::Ok) {
                    throw std::runtime_error("Net benchmark UDP batch receive failed");
                }
                for (const auto& item : receives) {
                    if (item.result.status == LikesProgram::Net::IoStatus::Ok) ++received;
                }
            }

            completed += static_cast<std::int64_t>(currentBatch);
        }

        const double elapsed = SafeElapsedSeconds(started);
        return Measurement{ "udp_batch_datagrams_per_second", "datagrams/s", elapsed, static_cast<double>(datagrams) / elapsed };
    }

    Measurement MeasureUdpMultiPeerDatagrams(std::int64_t datagrams) {
        LikesProgram::Net::Address any("127.0.0.1", 0); // 接收端和发送端都绑定本地回环。
        LikesProgram::Net::SocketType receiverFd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        LikesProgram::Net::SocketType senderAFd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        LikesProgram::Net::SocketType senderBFd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (receiverFd == LikesProgram::Net::kInvalidSocket
            || senderAFd == LikesProgram::Net::kInvalidSocket
            || senderBFd == LikesProgram::Net::kInvalidSocket) {
            throw std::runtime_error("Net benchmark UDP multi peer socket failed");
        }

        LikesProgram::Net::UdpTransport receiver(receiverFd);
        LikesProgram::Net::UdpTransport senderA(senderAFd);
        LikesProgram::Net::UdpTransport senderB(senderBFd);
        if (::bind(receiverFd, any.SockAddr(), any.Length()) != 0
            || ::bind(senderAFd, any.SockAddr(), any.Length()) != 0
            || ::bind(senderBFd, any.SockAddr(), any.Length()) != 0) {
            throw std::runtime_error("Net benchmark UDP multi peer bind failed");
        }
        if (!SetBenchNonBlocking(receiverFd) || !SetBenchNonBlocking(senderAFd) || !SetBenchNonBlocking(senderBFd)) {
            throw std::runtime_error("Net benchmark UDP multi peer nonblocking failed");
        }

        const auto receiverAddress = LikesProgram::Net::Address::GetLocalAddress(receiverFd);
        const auto senderAAddress = LikesProgram::Net::Address::GetLocalAddress(senderAFd);
        const auto senderBAddress = LikesProgram::Net::Address::GetLocalAddress(senderBFd);
        const std::uint8_t payloadA[] = { 'a' };
        const std::uint8_t payloadB[] = { 'b' };

        const auto started = Clock::now();
        std::int64_t sentCount = 0; // 已提交的 datagram 数，和接收进度交错推进。
        std::int64_t received = 0; // 已收到的 datagram 数。
        bool sawPeerA = false;     // 是否观察到 sender A 的地址。
        bool sawPeerB = false;     // 是否观察到 sender B 的地址。
        const bool completed = WaitUntil(
            [&]() {
                for (int burst = 0; burst < 16 && sentCount < datagrams; ++burst) {
                    auto& sender = (sentCount % 2 == 0) ? senderA : senderB; // 交替来源验证 peer-aware 语义。
                    const auto* payload = (sentCount % 2 == 0) ? payloadA : payloadB;
                    const auto sent = sender.SendTo(receiverAddress, payload, 1);
                    if (sent.status == LikesProgram::Net::IoStatus::WouldBlock) return false;
                    if (sent.status != LikesProgram::Net::IoStatus::Ok) {
                        throw std::runtime_error("Net benchmark UDP multi peer send failed");
                    }
                    ++sentCount;
                }

                LikesProgram::Net::Buffer buffer;
                LikesProgram::Net::Address peer;
                LikesProgram::Net::UdpReceiveDatagram item;
                item.buffer = &buffer;
                item.peer = &peer;

                while (received < sentCount) {
                    const auto read = receiver.ReadBatch(&item, 1);
                    if (read.status == LikesProgram::Net::IoStatus::WouldBlock) return false;
                    if (read.status != LikesProgram::Net::IoStatus::Ok) {
                        throw std::runtime_error("Net benchmark UDP multi peer receive failed");
                    }
                    if (item.result.status == LikesProgram::Net::IoStatus::Ok && peer.IsValid()) {
                        sawPeerA = sawPeerA || peer.Port() == senderAAddress.Port();
                        sawPeerB = sawPeerB || peer.Port() == senderBAddress.Port();
                        ++received;
                    }
                    buffer.RetrieveAll();
                }
                return received >= datagrams;
            },
            std::chrono::seconds(5));

        const double elapsed = SafeElapsedSeconds(started);
        if (!completed || !sawPeerA || !sawPeerB) {
            throw std::runtime_error("Net benchmark UDP multi peer validation failed");
        }
        return Measurement{ "udp_multi_peer_datagrams_per_second", "datagrams/s", elapsed, static_cast<double>(datagrams) / elapsed };
    }

    double Percentile(const std::vector<double>& sortedValues, double quantile) {
        if (sortedValues.empty()) return 0.0;

        const double rawIndex = quantile * static_cast<double>(sortedValues.size() - 1); // 线性插值下标。
        const auto lower = static_cast<std::size_t>(rawIndex);
        const auto upper = std::min<std::size_t>(lower + 1, sortedValues.size() - 1);
        const double ratio = rawIndex - static_cast<double>(lower); // 上下两个样本的插值比例。
        return sortedValues[lower] * (1.0 - ratio) + sortedValues[upper] * ratio;
    }

    LatencySummary MeasurePostTaskWakeupLatency(std::size_t samples) {
        LikesProgram::Net::EventLoop loop;
        std::atomic<bool> loopEntered{ false }; // 等待 loop 线程进入 Start。
        std::vector<double> latencies;
        latencies.reserve(samples);

        loop.SetPollTimeout(500);
        std::thread worker([&loop, &loopEntered]() {
            loopEntered.store(true, std::memory_order_release);
            loop.Start();
        });

        const bool started = WaitUntil(
            [&loopEntered]() {
                return loopEntered.load(std::memory_order_acquire);
            },
            std::chrono::seconds(1));
        if (!started) throw std::runtime_error("Net benchmark EventLoop did not start");
        std::this_thread::sleep_for(std::chrono::milliseconds(25)); // 给轮询线程进入阻塞态的时间。

        for (std::size_t i = 0; i < samples; ++i) {
            std::atomic<std::int64_t> elapsedUs{ -1 }; // 当前样本的唤醒延迟。
            const auto sentAt = Clock::now();
            loop.PostTask([sentAt, &elapsedUs]() {
                const auto doneAt = Clock::now(); // 在 loop 线程内记录任务真正执行时间。
                elapsedUs.store(
                    std::chrono::duration_cast<std::chrono::microseconds>(doneAt - sentAt).count(),
                    std::memory_order_release);
            });

            const bool completed = WaitUntil(
                [&elapsedUs]() {
                    return elapsedUs.load(std::memory_order_acquire) >= 0;
                },
                std::chrono::seconds(1));
            if (!completed) throw std::runtime_error("Net benchmark EventLoop PostTask timed out");
            latencies.push_back(static_cast<double>(elapsedUs.load(std::memory_order_acquire)));
        }

        loop.Shutdown();
        worker.join();

        std::sort(latencies.begin(), latencies.end());
        return LatencySummary{
            "post_task_wakeup_latency_microseconds",
            latencies.size(),
            Percentile(latencies, 0.50),
            Percentile(latencies, 0.95),
            Percentile(latencies, 0.99),
            latencies.empty() ? 0.0 : latencies.back()
        };
    }

    // 运行应用侧 TLS/DTLS Engine 合同 smoke，计数不接触 socket 或密码库。
    EngineContractSummary MeasureEngineContracts() {
        EngineContractSummary summary;

        LikesProgram::Net::TlsEngineFactory tlsFactory(
            []() {
                return std::make_unique<BenchmarkTlsEngine>();
            });
        if (!tlsFactory.InitializeSharedResources()) {
            throw std::runtime_error("TLS Engine benchmark shared initialization failed");
        }
        auto tlsEngine = tlsFactory.Create(); // 每轮只创建一个应用侧 TLS session
        if (!tlsEngine) throw std::runtime_error("TLS Engine benchmark creation failed");

        LikesProgram::Net::BufferChain tlsHandshakeOutput;
        const auto tlsStart = tlsEngine->StartHandshake(tlsHandshakeOutput);
        if (!tlsStart.Succeeded()
            || !tlsStart.HasAction(LikesProgram::Net::TlsAction::NeedCiphertext)) {
            throw std::runtime_error("TLS Engine benchmark handshake contract failed");
        }
        summary.tlsCiphertextBytes += tlsHandshakeOutput.ReadableBytes();

        LikesProgram::Net::BufferChain tlsCiphertextInput;
        LikesProgram::Net::Buffer tlsCiphertext(0); // 模拟 socket completion 交付的密文
        tlsCiphertext.Append("tls-peer", sizeof("tls-peer") - 1);
        tlsCiphertextInput.Append(std::move(tlsCiphertext));
        LikesProgram::Net::BufferChain tlsPlaintextOutput;
        LikesProgram::Net::BufferChain tlsResponseOutput;
        const auto tlsRead = tlsEngine->ConsumeCiphertext(
            tlsCiphertextInput, tlsPlaintextOutput, tlsResponseOutput);
        if (!tlsRead.Succeeded()
            || !tlsRead.HasAction(LikesProgram::Net::TlsAction::PlaintextReady)) {
            throw std::runtime_error("TLS Engine benchmark ciphertext contract failed");
        }
        summary.tlsPlaintextBytes += tlsPlaintextOutput.ReadableBytes();
        summary.tlsCiphertextBytes += tlsResponseOutput.ReadableBytes();

        LikesProgram::Net::BufferChain tlsPlaintextInput;
        LikesProgram::Net::Buffer tlsApplicationData(0); // 模拟业务层提交的明文
        tlsApplicationData.Append("tls-app", sizeof("tls-app") - 1);
        tlsPlaintextInput.Append(std::move(tlsApplicationData));
        LikesProgram::Net::BufferChain tlsApplicationCiphertext;
        const auto tlsWrite = tlsEngine->ConsumePlaintext(
            tlsPlaintextInput, tlsApplicationCiphertext);
        if (!tlsWrite.Succeeded()
            || !tlsWrite.HasAction(LikesProgram::Net::TlsAction::CiphertextReady)) {
            throw std::runtime_error("TLS Engine benchmark plaintext contract failed");
        }
        summary.tlsCiphertextBytes += tlsApplicationCiphertext.ReadableBytes();

        LikesProgram::Net::BufferChain tlsCloseOutput;
        const auto tlsClose = tlsEngine->Shutdown(tlsCloseOutput);
        if (!tlsClose.Succeeded()
            || !tlsClose.HasAction(LikesProgram::Net::TlsAction::CloseTransport)
            || tlsEngine->State() != LikesProgram::Net::TlsState::Closed) {
            throw std::runtime_error("TLS Engine benchmark close contract failed");
        }
        summary.tlsCiphertextBytes += tlsCloseOutput.ReadableBytes();

        LikesProgram::Net::DtlsEngineFactory dtlsFactory(
            LikesProgram::Net::DtlsRole::Client,
            [](const LikesProgram::Net::Address&, const LikesProgram::Net::Address&, std::size_t) {
                return std::make_unique<BenchmarkDtlsEngine>();
            });
        if (!dtlsFactory.InitializeSharedResources()) {
            throw std::runtime_error("DTLS Engine benchmark shared initialization failed");
        }
        auto dtlsEngine = dtlsFactory.Create(
            LikesProgram::Net::Address("127.0.0.1", 4433),
            LikesProgram::Net::Address("127.0.0.1", 4434),
            1200); // 1200 字节只作为 Engine 创建契约的保护值
        if (!dtlsEngine) throw std::runtime_error("DTLS Engine benchmark creation failed");

        LikesProgram::Net::DtlsDatagramBatch dtlsHandshakeOutput;
        const auto dtlsStart = dtlsEngine->StartHandshake(dtlsHandshakeOutput);
        if (!dtlsStart.Succeeded()
            || !dtlsStart.HasAction(LikesProgram::Net::DtlsAction::CiphertextReady)) {
            throw std::runtime_error("DTLS Engine benchmark handshake contract failed");
        }
        summary.dtlsDatagrams += dtlsHandshakeOutput.Count();
        summary.dtlsPayloadBytes += dtlsHandshakeOutput.TotalBytes();

        LikesProgram::Net::Buffer dtlsCiphertext(0); // 模拟一个完整 peer 密文数据报
        dtlsCiphertext.Append("dtls-peer", sizeof("dtls-peer") - 1);
        LikesProgram::Net::DtlsDatagramBatch dtlsPlaintextOutput;
        LikesProgram::Net::DtlsDatagramBatch dtlsResponseOutput;
        const auto dtlsRead = dtlsEngine->ConsumeCiphertext(
            dtlsCiphertext, dtlsPlaintextOutput, dtlsResponseOutput);
        if (!dtlsRead.Succeeded()
            || !dtlsRead.HasAction(LikesProgram::Net::DtlsAction::PlaintextReady)) {
            throw std::runtime_error("DTLS Engine benchmark ciphertext contract failed");
        }
        summary.dtlsDatagrams += dtlsPlaintextOutput.Count();
        summary.dtlsPayloadBytes += dtlsPlaintextOutput.TotalBytes();

        LikesProgram::Net::Buffer dtlsPlaintext(0); // 模拟业务层提交的一个明文数据报
        dtlsPlaintext.Append("dtls-app", sizeof("dtls-app") - 1);
        LikesProgram::Net::DtlsDatagramBatch dtlsApplicationCiphertext;
        const auto dtlsWrite = dtlsEngine->ConsumePlaintext(
            dtlsPlaintext, dtlsApplicationCiphertext);
        if (!dtlsWrite.Succeeded()
            || !dtlsWrite.HasAction(LikesProgram::Net::DtlsAction::CiphertextReady)) {
            throw std::runtime_error("DTLS Engine benchmark plaintext contract failed");
        }
        summary.dtlsDatagrams += dtlsApplicationCiphertext.Count();
        summary.dtlsPayloadBytes += dtlsApplicationCiphertext.TotalBytes();

        LikesProgram::Net::DtlsDatagramBatch dtlsRetryOutput;
        const auto dtlsTimeout = dtlsEngine->HandleTimeout(dtlsRetryOutput);
        if (!dtlsTimeout.Succeeded()
            || !dtlsTimeout.HasAction(LikesProgram::Net::DtlsAction::ArmRetransmitTimer)) {
            throw std::runtime_error("DTLS Engine benchmark timeout contract failed");
        }
        summary.dtlsDatagrams += dtlsRetryOutput.Count();
        summary.dtlsPayloadBytes += dtlsRetryOutput.TotalBytes();

        LikesProgram::Net::DtlsDatagramBatch dtlsCloseOutput;
        const auto dtlsClose = dtlsEngine->Shutdown(dtlsCloseOutput);
        if (!dtlsClose.Succeeded()
            || !dtlsClose.HasAction(LikesProgram::Net::DtlsAction::CloseSession)
            || dtlsEngine->State() != LikesProgram::Net::DtlsState::Closed) {
            throw std::runtime_error("DTLS Engine benchmark close contract failed");
        }
        summary.dtlsDatagrams += dtlsCloseOutput.Count();
        summary.dtlsPayloadBytes += dtlsCloseOutput.TotalBytes();

        return summary;
    }

    Measurement MeasureShortStressRounds(std::size_t rounds) {
        const auto started = Clock::now();
        for (std::size_t i = 0; i < rounds; ++i) {
            // 短时压力循环覆盖重复启停和 TCP/UDP 双路径，不把它当作长稳替代品。
            (void)MeasureTcpEchoBytes(8 * 1024);
            (void)MeasureUdpBatchDatagrams(64, 8);
        }

        const double elapsed = SafeElapsedSeconds(started);
        return Measurement{ "short_stress_rounds_per_second", "rounds/s", elapsed, static_cast<double>(rounds) / elapsed };
    }

    void PrintEnvironment() {
        const auto poller = LikesProgram::Net::CreateDefaultPoller(nullptr); // 构造期诊断与实际默认工厂使用同一选择策略。
        std::cout << "benchmark_scope=raw_event_connection_foundation\n";
        std::cout << "comparison_note=not_full_http_reverse_proxy\n";
        std::cout << "platform=" << PlatformName() << '\n';
        std::cout << "backend=" << poller->BackendName() << '\n';
        std::cout << "hardware_concurrency=" << std::thread::hardware_concurrency() << '\n';
        std::cout << "matrix=tcp_echo,tcp_multi_client,tcp_full_duplex,tcp_proxy_full_duplex,tcp_idle,tcp_slow_reader,udp_echo,udp_sendto,udp_batch,udp_multi_peer,tls_engine_contract,dtls_engine_contract,post_task_wakeup,short_stress\n";
    }

    void PrintMeasurement(const Measurement& measurement) {
        std::cout << measurement.name
            << " seconds=" << measurement.seconds
            << " throughput=" << measurement.throughput
            << " unit=" << measurement.unit
            << '\n';
    }

    void PrintGauge(const char* name, std::int64_t value, const char* unit) {
        std::cout << name
            << " value=" << value
            << " unit=" << unit
            << '\n';
    }

    void PrintLatencySummary(const LatencySummary& summary) {
        std::cout << summary.name
            << " samples=" << summary.samples
            << " p50=" << summary.p50
            << " p95=" << summary.p95
            << " p99=" << summary.p99
            << " max=" << summary.max
            << " unit=us"
            << '\n';
    }

    void PrintSlowReaderSummary(const SlowReaderSummary& summary) {
        std::cout << "tcp_slow_reader_backpressure"
            << " seconds=" << summary.seconds
            << " submitted_bytes=" << summary.submittedBytes
            << " maximum_pending_bytes=" << summary.maximumPendingBytes
            << " high_watermark_events=" << summary.highWatermarkEvents
            << " overflow_events=" << summary.overflowEvents
            << '\n';
    }

    // 输出脚本稳定解析的代理吞吐与资源收敛单行指标。
    void PrintProxyRelaySummary(const LikesProgram::Net::Benchmarks::ProxyRelayStats& stats) {
        std::cout << LikesProgram::Net::Benchmarks::FormatProxyRelayStats(stats);
    }

    // 输出 TLS/DTLS 应用侧 Engine 合同计数，避免与具体密码性能混淆。
    void PrintEngineContractSummary(const EngineContractSummary& summary) {
        std::cout << "tls_engine_contract sessions=1 start_handshake=1 "
            << "consume_ciphertext=1 consume_plaintext=1 shutdown=1 "
            << "ciphertext_bytes=" << summary.tlsCiphertextBytes
            << " plaintext_bytes=" << summary.tlsPlaintextBytes
            << " state=closed unit=contract\n";
        std::cout << "dtls_engine_contract sessions=1 start_handshake=1 "
            << "consume_ciphertext=1 consume_plaintext=1 timeout=1 shutdown=1 "
            << "datagrams=" << summary.dtlsDatagrams
            << " payload_bytes=" << summary.dtlsPayloadBytes
            << " state=closed unit=contract\n";
    }
}

int main() {
    try {
        PrintEnvironment();

        PrintMeasurement(MeasureTcpEchoBytes(64 * 1024));
        PrintMeasurement(MeasureTcpMultiClientBytes(4, 32 * 1024));
        PrintMeasurement(MeasureTcpFullDuplexBytes(1024 * 1024));
        LikesProgram::Net::Benchmarks::ProxyRelayOptions proxyOptions;
        proxyOptions.payloadBytes = 256 * 1024;
        proxyOptions.upstreamStartDelay = std::chrono::milliseconds(5);
        PrintProxyRelaySummary(
            LikesProgram::Net::Benchmarks::RunTcpProxyRelayBenchmark(proxyOptions));

        constexpr std::int64_t idleConnections = 16;
        PrintMeasurement(MeasureTcpIdleConnections(static_cast<std::size_t>(idleConnections)));
        PrintGauge("tcp_idle_connections_established", idleConnections, "connections");
        PrintSlowReaderSummary(MeasureTcpSlowReaderBackpressure());

        PrintMeasurement(MeasureUdpEchoDatagrams(128));
        PrintMeasurement(MeasureUdpSendToDatagrams(256));
        PrintMeasurement(MeasureUdpBatchDatagrams(512, 16));
        PrintMeasurement(MeasureUdpMultiPeerDatagrams(128));
        PrintEngineContractSummary(MeasureEngineContracts());
        PrintLatencySummary(MeasurePostTaskWakeupLatency(64));
        PrintMeasurement(MeasureShortStressRounds(2));

        return 0;
    }
    catch (const std::exception& ex) {
        std::cerr << "Benchmark failed: " << ex.what() << '\n';
        return 1;
    }
}
