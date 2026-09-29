#include <LikesProgram/Net/Net.hpp>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
    class ConsumerTlsEngine final : public LikesProgram::Net::TlsEngine {
    public:
        // 外部 Engine 可产生握手密文，不需要接触 Net 持有的 socket。
        LikesProgram::Net::TlsResult StartHandshake(
            LikesProgram::Net::BufferChain&) override {
            return { LikesProgram::Net::TlsAction::NeedCiphertext, 0 };
        }

        // 外部 Engine 通过 BufferChain 消费 socket completion 密文。
        LikesProgram::Net::TlsResult ConsumeCiphertext(
            LikesProgram::Net::BufferChain& ciphertextInput,
            LikesProgram::Net::BufferChain&,
            LikesProgram::Net::BufferChain&) override {
            ciphertextInput.Consume(ciphertextInput.ReadableBytes());
            m_state = LikesProgram::Net::TlsState::Active;
            return { LikesProgram::Net::TlsAction::None, 0 };
        }

        // 外部 Engine 通过 BufferChain 接收业务明文并产生密文。
        LikesProgram::Net::TlsResult ConsumePlaintext(
            LikesProgram::Net::BufferChain& plaintextInput,
            LikesProgram::Net::BufferChain&) override {
            plaintextInput.Consume(plaintextInput.ReadableBytes());
            return { LikesProgram::Net::TlsAction::CiphertextReady, 0 };
        }

        // 外部 Engine 决定何时请求关闭底层 transport。
        LikesProgram::Net::TlsResult Shutdown(
            LikesProgram::Net::BufferChain&) override {
            m_state = LikesProgram::Net::TlsState::Closed;
            return { LikesProgram::Net::TlsAction::CloseTransport, 0 };
        }

        // 返回当前外部 Engine 状态。
        LikesProgram::Net::TlsState State() const noexcept override {
            return m_state;
        }

        // 当前检查不协商 ALPN。
        const char* NegotiatedProtocol() const noexcept override {
            return "";
        }

    private:
        LikesProgram::Net::TlsState m_state = LikesProgram::Net::TlsState::Handshaking; // 外部会话状态
    };

    class ConsumerDtlsEngine final : public LikesProgram::Net::DtlsEngine {
    public:
        LikesProgram::Net::DtlsResult StartHandshake(
            LikesProgram::Net::DtlsDatagramBatch& ciphertextOutput) override {
            LikesProgram::Net::Buffer flight(0); // 安装态完整 ciphertext 数据报
            flight.Append("dtls", 4);
            ciphertextOutput.Append(std::move(flight));
            return { LikesProgram::Net::DtlsAction::CiphertextReady, 0, 0 };
        }

        LikesProgram::Net::DtlsResult ConsumeCiphertext(
            LikesProgram::Net::Buffer& input,
            LikesProgram::Net::DtlsDatagramBatch&,
            LikesProgram::Net::DtlsDatagramBatch&) override {
            input.RetrieveAll();
            m_state = LikesProgram::Net::DtlsState::Active;
            return {};
        }

        LikesProgram::Net::DtlsResult ConsumePlaintext(
            LikesProgram::Net::Buffer& input,
            LikesProgram::Net::DtlsDatagramBatch&) override {
            input.RetrieveAll();
            return {};
        }

        LikesProgram::Net::DtlsResult HandleTimeout(
            LikesProgram::Net::DtlsDatagramBatch&) override {
            return {};
        }

        LikesProgram::Net::DtlsResult Shutdown(
            LikesProgram::Net::DtlsDatagramBatch&) override {
            m_state = LikesProgram::Net::DtlsState::Closed;
            return { LikesProgram::Net::DtlsAction::CloseSession, 0, 0 };
        }

        LikesProgram::Net::DtlsState State() const noexcept override { return m_state; }
        const char* NegotiatedProtocol() const noexcept override { return "consumer-dtls"; }

    private:
        LikesProgram::Net::DtlsState m_state = LikesProgram::Net::DtlsState::Handshaking; // 外部 peer 状态
    };

    class ConsumerBackpressureConnection : public LikesProgram::Net::Connection {
    public:
        // 外部消费方通过继承 Connection 接收写队列水位事件。
        ConsumerBackpressureConnection(
            LikesProgram::Net::SocketType fd,
            LikesProgram::Net::EventLoop* loop)
            : Connection(fd, loop) {
        }

    protected:
        // 高水位暂停读，低水位再恢复，形成 TCP 连接级背压。
        void OnWriteHighWatermark(std::size_t) override {
            PauseReading();
        }
        void OnWriteLowWatermark(std::size_t) override {
            ResumeReading();
        }
        void OnWriteQueueOverflow(std::size_t) override {
            ForceClose();
        }
    };
}

int main() {
    LikesProgram::Net::Buffer buffer;
    std::uint8_t* writeBegin = buffer.PrepareWrite(3);
    writeBegin[0] = 'n';
    writeBegin[1] = 'e';
    writeBegin[2] = 't';
    buffer.HasWritten(3);

    ConsumerBackpressureConnection backpressureConnection(LikesProgram::Net::kInvalidSocket, nullptr);
    backpressureConnection.SetWriteWatermark(64 * 1024, 16 * 1024);
    backpressureConnection.SetMaxPendingWriteBytes(4 * 1024 * 1024);
    backpressureConnection.SetTlsHandshakeTimeout(std::chrono::seconds(5));
    LikesProgram::Net::EventLoopGroup workerGroup(2);

    if (buffer.AsStringView() != "net") return 3;
    if (!backpressureConnection.IsConnected()) return 6;
    if (workerGroup.Size() != 2 || workerGroup.IsRunning()) return 7;

    workerGroup.Start();
    LikesProgram::Net::EventLoop* firstWorker = workerGroup.NextLoop();  // 外部消费方可轮询取得 worker loop
    LikesProgram::Net::EventLoop* secondWorker = workerGroup.NextLoop(); // 2 worker 场景应分发到不同 loop
    if (!workerGroup.IsRunning()) return 8;
    if (firstWorker == nullptr || secondWorker == nullptr || firstWorker == secondWorker) return 9;
    workerGroup.Shutdown();
    if (workerGroup.IsRunning()) return 10;

    auto poller = LikesProgram::Net::CreateDefaultPoller(nullptr); // 外部消费方可观察默认 Poller 后端
    if (!poller || poller->BackendName() == nullptr) return 20;
#if defined(__linux__)
    const bool ioUringBackend = std::strcmp(
        poller->BackendName(),
        "io_uring-multishot-provided-buffer") == 0;
    const bool epollBackend = std::strcmp(
        poller->BackendName(),
        "epoll-level-completion") == 0;
    if (!ioUringBackend && !epollBackend) return 21;
    const char* requestedBackend = std::getenv("LIKESPROGRAM_NET_BACKEND");
    const bool forcedEpoll = requestedBackend != nullptr
        && std::strcmp(requestedBackend, "epoll") == 0;
    if (forcedEpoll ? !epollBackend : !ioUringBackend) return 21;
    const LikesProgram::Net::CompletionStats completionStats = poller->GetCompletionStats(); // 安装态诊断池规格
    if (epollBackend) {
        if (completionStats.providedBufferCount != 0
            || completionStats.providedBufferSize != 0
            || completionStats.currentAvailableBuffers != 0
            || completionStats.receiveBundleEnabled
            || completionStats.datagramMultishotEnabled
            || completionStats.datagramProvidedBufferCount != 0
            || completionStats.datagramActiveBufferLeases != 0) return 28;
    }
    else {
        if (completionStats.providedBufferCount == 0
            || completionStats.providedBufferSize == 0) return 28;
        if (completionStats.currentAvailableBuffers
            != completionStats.providedBufferCount) return 29;
    }
    if (completionStats.receiveBundleCompletions != 0
        || completionStats.receiveBundleBuffers != 0
        || completionStats.maximumReceiveBundleBuffers != 0) return 30;
#elif defined(_WIN32)
    if (std::strcmp(poller->BackendName(), "iocp-overlapped") != 0) return 21;
    const LikesProgram::Net::CompletionStats completionStats = poller->GetCompletionStats(); // 安装态诊断池规格
    if (completionStats.providedBufferCount != 0
        || completionStats.providedBufferSize != 0
        || completionStats.currentAvailableBuffers != 0
        || completionStats.receiveBundleEnabled
        || completionStats.datagramMultishotEnabled
        || completionStats.datagramProvidedBufferCount != 0
        || completionStats.datagramActiveBufferLeases != 0
        || completionStats.receiveBundleCompletions != 0
        || completionStats.receiveBundleBuffers != 0
        || completionStats.maximumReceiveBundleBuffers != 0) return 28;
#else
    return 21;
#endif

    LikesProgram::Net::ConnectionPoolOptions poolOptions;
    poolOptions.remoteAddress = LikesProgram::Net::Address("127.0.0.1", 9);
    poolOptions.transportKind = LikesProgram::Net::TransportKind::Udp;
    bool poolRejectedUdp = false; // 连接池是 TCP-only 子能力，UDP 必须在构造期被拒绝。
    try {
        LikesProgram::Net::ConnectionPool pool(poolOptions);
        (void)pool;
    }
    catch (const std::invalid_argument&) {
        poolRejectedUdp = true;
    }
    if (!poolRejectedUdp) return 11;

    LikesProgram::Net::ConnectionFactory factory(
        [](LikesProgram::Net::SocketType, LikesProgram::Net::EventLoop*) {
            return std::shared_ptr<LikesProgram::Net::Connection>{};
        });
    if (!factory) return 14;

    std::atomic<int> tlsSharedInitCount{ 0 }; // TLS Factory 复制时共享初始化状态
    LikesProgram::Net::TlsEngineFactory tlsFactory(
        []() {
            return std::make_unique<ConsumerTlsEngine>();
        },
        [&tlsSharedInitCount]() {
            tlsSharedInitCount.fetch_add(1);
            return true;
        });
    LikesProgram::Net::TlsEngineFactory copiedTlsFactory(tlsFactory); // 外部用户无需实现 Factory 子类
    backpressureConnection.SetTlsEngineFactory(copiedTlsFactory); // Connection 只接收 Engine Factory，不接收 Transport
    if (!backpressureConnection.HasTlsEngineFactory()) return 27;
    if (!tlsFactory.InitializeSharedResources()) return 22;
    if (!copiedTlsFactory.InitializeSharedResources()) return 23;
    if (tlsSharedInitCount.load() != 1) return 24;

    auto tlsEngine = copiedTlsFactory.Create(); // 每个连接取得独立 TLS 会话对象
    if (!tlsEngine) return 25;
    LikesProgram::Net::BufferChain tlsCiphertext; // Engine 与 Net 之间只交换 BufferChain
    const LikesProgram::Net::TlsResult tlsStart = tlsEngine->StartHandshake(tlsCiphertext);
    if (!tlsStart.Succeeded()
        || !tlsStart.HasAction(LikesProgram::Net::TlsAction::NeedCiphertext)) {
        return 26;
    }

    std::atomic<int> dtlsSharedInitCount{ 0 }; // 复制 Server Factory 共享一次性资源状态
    LikesProgram::Net::DtlsEngineFactory dtlsFactory(
        LikesProgram::Net::DtlsRole::Server,
        [](const LikesProgram::Net::Address&, const LikesProgram::Net::Address&, std::size_t) {
            return std::make_unique<ConsumerDtlsEngine>();
        },
        [&dtlsSharedInitCount]() {
            dtlsSharedInitCount.fetch_add(1);
            return true;
        });
    LikesProgram::Net::DtlsEngineFactory copiedDtlsFactory(dtlsFactory); // 安装态 PImpl 复制
    if (!dtlsFactory.InitializeSharedResources()
        || !copiedDtlsFactory.InitializeSharedResources()
        || dtlsSharedInitCount.load() != 1
        || copiedDtlsFactory.Role() != LikesProgram::Net::DtlsRole::Server) return 31;
    auto dtlsEngine = copiedDtlsFactory.Create(
        LikesProgram::Net::Address(), LikesProgram::Net::Address(), 1200);
    if (!dtlsEngine) return 32;
    LikesProgram::Net::DtlsDatagramBatch dtlsCiphertext; // 安装态数据报边界容器
    const auto dtlsStart = dtlsEngine->StartHandshake(dtlsCiphertext);
    if (!dtlsStart.HasAction(LikesProgram::Net::DtlsAction::CiphertextReady)
        || dtlsCiphertext.Count() != 1 || dtlsCiphertext.At(0).AsStringView() != "dtls") return 33;
    LikesProgram::Net::Buffer taken(0);
    if (!dtlsCiphertext.TakeFront(taken) || taken.AsStringView() != "dtls"
        || !dtlsCiphertext.Empty()) return 34;
    LikesProgram::Net::Connection dtlsConnection(
        LikesProgram::Net::kInvalidSocket,
        nullptr,
        LikesProgram::Net::TransportKind::Udp);
    dtlsConnection.SetDtlsEngineFactory(copiedDtlsFactory);
    dtlsConnection.SetDtlsMaximumCiphertextDatagramBytes(1200);
    dtlsConnection.SetDtlsHandshakeTimeout(std::chrono::seconds(30));
    dtlsConnection.SetDtlsSessionIdleTimeout(std::chrono::minutes(5));
    dtlsConnection.SetDtlsSessionLimits(1024, 256, 256 * 1024);
    if (!dtlsConnection.HasDtlsEngineFactory()) return 35;


    std::cout << LikesProgram::Net::PackageName()
        << " consumer check passed\n";
    return 0;
}
