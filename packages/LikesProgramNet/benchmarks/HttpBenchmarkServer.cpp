#include <LikesProgram/Net/Net.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
    std::atomic<bool> g_running{ true }; // 进程级运行标记，由信号处理函数关闭。

    constexpr std::string_view kHttpBody = "LikesProgramNet\n";
    static_assert(kHttpBody.size() == 16);

    constexpr std::string_view kHttpResponse =
        "HTTP/1.1 200 OK\r\n"
        "Server: LikesProgramNet\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 16\r\n"
        "Connection: keep-alive\r\n"
        "\r\n"
        "LikesProgramNet\n";

    struct WorkerStats {
        std::size_t index = 0; // 当前进程内的 worker 诊断编号
        std::thread::id threadId{}; // 记录真实 loop 线程，便于排查 worker 分布
        std::atomic<unsigned long long> requests{ 0 }; // 当前 worker 处理的响应请求数
        std::atomic<unsigned long long> activeConnections{ 0 }; // 当前 worker 仍持有的活跃连接数
    };

    void HandleSignal(int) {
        g_running.store(false, std::memory_order_release);
    }

    std::atomic<bool> g_collectWorkerStats{ false }; // 由环境变量开启，默认不污染普通基线
    std::mutex g_workerStatsMutex; // 保护 worker stats 注册表
    std::vector<std::unique_ptr<WorkerStats>> g_workerStats; // worker 诊断条目，生命周期到进程结束
    thread_local WorkerStats* g_currentWorkerStats = nullptr; // 当前 loop 线程绑定的诊断条目

    enum class ServerMode {
        Standard,
        NoWatermark,
        RawRead,
        RawReadNoWatermark
    };

    const char* ServerModeName(ServerMode mode) noexcept {
        switch (mode) {
        case ServerMode::Standard:
            return "standard";
        case ServerMode::NoWatermark:
            return "no-watermark";
        case ServerMode::RawRead:
            return "raw";
        case ServerMode::RawReadNoWatermark:
            return "raw-no-watermark";
        }

        return "standard";
    }

    bool IsPositiveInteger(std::string_view text) noexcept {
        if (text.empty()) return false;

        for (char ch : text) {
            // 命令行只接受 ASCII 十进制 worker 数。
            if (ch < '0' || ch > '9') return false;
        }

        return true;
    }

    bool ModeUsesBackpressure(ServerMode mode) noexcept {
        return mode == ServerMode::Standard
            || mode == ServerMode::RawRead;
    }

    bool ModeUsesHeaderScan(ServerMode mode) noexcept {
        return mode == ServerMode::Standard
            || mode == ServerMode::NoWatermark;
    }

    bool ParseWorkerStatsEnabled() noexcept {
#ifdef _WIN32
        char* rawValue = nullptr; // _dupenv_s 分配的环境变量副本，需要手动释放
        std::size_t valueLength = 0; // 环境变量字节长度，当前只用于判断读取成功
        if (_dupenv_s(&rawValue, &valueLength, "LP_HTTP_BENCH_WORKER_STATS") != 0 || rawValue == nullptr) {
            return false;
        }

        const std::string text(rawValue);
        std::free(rawValue);
#else
        const char* value = std::getenv("LP_HTTP_BENCH_WORKER_STATS"); // 仅 benchmark 进程读取
        if (value == nullptr) return false;

        const std::string_view text(value);
#endif
        return text == "1" || text == "true" || text == "TRUE" || text == "on" || text == "ON";
    }

    WorkerStats* CurrentWorkerStats() {
        if (!g_collectWorkerStats.load(std::memory_order_acquire)) return nullptr;
        if (g_currentWorkerStats != nullptr) return g_currentWorkerStats;

        std::lock_guard<std::mutex> lock(g_workerStatsMutex); // 首次进入当前线程时登记一次
        auto stats = std::make_unique<WorkerStats>();
        stats->index = g_workerStats.size();
        stats->threadId = std::this_thread::get_id();
        g_currentWorkerStats = stats.get();
        g_workerStats.push_back(std::move(stats));
        return g_currentWorkerStats;
    }

    void DumpWorkerStats(const char* phase) {
        if (!g_collectWorkerStats.load(std::memory_order_acquire)) return;

        std::lock_guard<std::mutex> lock(g_workerStatsMutex); // 输出时冻结条目列表，计数仍可并发读取
        std::cout << "worker_stats phase=" << phase
            << " workers=" << g_workerStats.size()
            << '\n';
        for (const auto& stats : g_workerStats) {
            if (!stats) continue;
            std::cout << "worker_stats"
                << " index=" << stats->index
                << " thread_id=" << stats->threadId
                << " requests=" << stats->requests.load(std::memory_order_relaxed)
                << " active_connections=" << stats->activeConnections.load(std::memory_order_relaxed)
                << '\n';
        }
        std::cout.flush();
    }

    template <bool CollectWorkerStats>
    struct WorkerStatsSlot {
    };

    template <>
    struct WorkerStatsSlot<true> {
        WorkerStats* m_stats = nullptr; // 当前连接归属 worker 的诊断条目，不拥有生命周期
    };

    template <bool CollectWorkerStats>
    class FixedHttpConnection final
        : public LikesProgram::Net::Connection,
          private WorkerStatsSlot<CollectWorkerStats> {
    public:
        // 构造固定响应 HTTP 连接，业务层只做最小请求边界识别。
        FixedHttpConnection(
            LikesProgram::Net::SocketType fd,
            LikesProgram::Net::EventLoop* loop,
            ServerMode mode)
            : Connection(fd, loop),
              m_mode(mode) {
            if (ModeUsesBackpressure(m_mode)) {
                // 标准模式保留背压保护，诊断模式可关闭以隔离水位开销。
                SetWriteWatermark(256 * 1024, 64 * 1024);
                SetMaxPendingWriteBytes(2 * 1024 * 1024);
            }
        }

    protected:
        void OnConnected() override {
            if constexpr (CollectWorkerStats) {
                this->m_stats = CurrentWorkerStats();
                if (this->m_stats != nullptr) {
                    this->m_stats->activeConnections.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }

        void OnMessage(LikesProgram::Net::Buffer& in) override {
            if (!ModeUsesHeaderScan(m_mode)) {
                // raw 诊断模式只验证连接/读写成本，不把 header 扫描混入热路径。
                if (in.ReadableBytes() == 0) return;
                RecordRequest();
                Send(kHttpResponse.data(), kHttpResponse.size());
                in.RetrieveAll();
                return;
            }

            // ApacheBench keep-alive 不做 pipeline，但这里仍按完整 header 边界循环处理。
            std::string_view view = in.AsStringView();
            for (;;) {
                const std::size_t headerEnd = view.find("\r\n\r\n");
                if (headerEnd == std::string_view::npos) return;

                RecordRequest();
                Send(kHttpResponse.data(), kHttpResponse.size());
                in.Consume(headerEnd + 4);
                view = in.AsStringView();
            }
        }

        void OnWriteHighWatermark(std::size_t) override {
            if (!ModeUsesBackpressure(m_mode)) return;
            PauseReading();
        }

        void OnWriteLowWatermark(std::size_t) override {
            if (!ModeUsesBackpressure(m_mode)) return;
            ResumeReading();
        }

        void OnWriteQueueOverflow(std::size_t) override {
            if (!ModeUsesBackpressure(m_mode)) return;
            ForceClose();
        }

        void OnClosed() override {
            if constexpr (CollectWorkerStats) {
                if (this->m_stats != nullptr) {
                    this->m_stats->activeConnections.fetch_sub(1, std::memory_order_relaxed);
                    this->m_stats = nullptr;
                }
            }
        }

    private:
        void RecordRequest() noexcept {
            if constexpr (CollectWorkerStats) {
                if (this->m_stats != nullptr) {
                    this->m_stats->requests.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }

        ServerMode m_mode = ServerMode::Standard; // 当前连接使用的诊断模式
    };

    std::uint16_t ParsePort(int argc, char** argv) {
        if (argc < 2) return 18080;

        const long value = std::strtol(argv[1], nullptr, 10);
        if (value <= 0 || value > 65535) {
            throw std::runtime_error("Invalid port");
        }
        return static_cast<std::uint16_t>(value);
    }

    unsigned int ParseWorkers(int argc, char** argv) {
        const unsigned int hardwareWorkers = std::max(1u, std::thread::hardware_concurrency()); // 默认使用全部硬件线程。
        if (argc < 3) return hardwareWorkers;
        if (!IsPositiveInteger(argv[2])) return hardwareWorkers;

        const long value = std::strtol(argv[2], nullptr, 10);
        if (value <= 0 || value > 1024) {
            throw std::runtime_error("Invalid workers");
        }

        return static_cast<unsigned int>(value);
    }

    ServerMode ParseMode(int argc, char** argv) {
        if (argc < 3) return ServerMode::Standard;

        const int modeIndex = IsPositiveInteger(argv[2]) ? 3 : 2; // 支持 port mode 与 port workers mode 两种形式。
        if (argc <= modeIndex) return ServerMode::Standard;

        const std::string_view text = argv[modeIndex];
        if (text == "standard") return ServerMode::Standard;
        if (text == "no-watermark") return ServerMode::NoWatermark;
        if (text == "raw") return ServerMode::RawRead;
        if (text == "raw-no-watermark") return ServerMode::RawReadNoWatermark;

        throw std::runtime_error("Invalid mode");
    }

    std::string ParseBindAddress(int argc, char** argv) {
        if (argc < 3) return "127.0.0.1";

        const int bindIndex = IsPositiveInteger(argv[2]) ? 4 : 3; // 对齐 port workers mode bind 与 port mode bind
        if (argc <= bindIndex || argv[bindIndex] == nullptr || argv[bindIndex][0] == '\0') {
            // 默认仍只监听回环地址，分离式压测必须显式开放 VM 网卡。
            return "127.0.0.1";
        }

        return argv[bindIndex]; // benchmark 工具把地址合法性继续交给 Net::Address 校验
    }
}

int main(int argc, char** argv) {
    try {
        std::signal(SIGINT, HandleSignal);
        std::signal(SIGTERM, HandleSignal);

        const std::uint16_t port = ParsePort(argc, argv);
        const ServerMode mode = ParseMode(argc, argv);
        const std::string bindAddress = ParseBindAddress(argc, argv); // 默认回环，可选分离式客户端
        g_collectWorkerStats.store(ParseWorkerStatsEnabled(), std::memory_order_release);
        const bool collectWorkerStats = g_collectWorkerStats.load(std::memory_order_acquire);
        LikesProgram::Net::Server server(
            LikesProgram::Net::Address(bindAddress, port),
            [mode, collectWorkerStats](LikesProgram::Net::SocketType fd, LikesProgram::Net::EventLoop* loop)
                -> std::shared_ptr<LikesProgram::Net::Connection> {
                if (collectWorkerStats) {
                    return std::make_shared<FixedHttpConnection<true>>(fd, loop, mode);
                }
                return std::make_shared<FixedHttpConnection<false>>(fd, loop, mode);
            });

        const unsigned int workers = ParseWorkers(argc, argv);
        server.SetWorkerThreads(workers);
        server.Start();

        std::cout << "likesprogram_http_benchmark_server ready"
            << " bind=" << bindAddress
            << " port=" << port
            << " workers=" << workers
            << " mode=" << ServerModeName(mode)
            << " worker_stats=" << (g_collectWorkerStats.load(std::memory_order_acquire) ? "on" : "off")
            << '\n';
        std::cout.flush();

        while (g_running.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        DumpWorkerStats("before_shutdown");
        server.Shutdown();
        DumpWorkerStats("after_shutdown");
        return 0;
    }
    catch (const std::exception& ex) {
        std::cerr << "HttpBenchmarkServer failed: " << ex.what() << '\n';
        return 1;
    }
}
