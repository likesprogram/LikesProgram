#include <LikesProgram/Logging/Logger.hpp>
#include <LikesProgram/Logging/sinks/FileSink.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <source_location>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#ifdef _MSC_VER
#define LP_BENCH_NOINLINE __declspec(noinline)
#else
#define LP_BENCH_NOINLINE __attribute__((noinline))
#endif

namespace {
    std::atomic<std::uint64_t> g_probe{ 0 }; // 防止编译器把微基准循环整体消除

    std::uint64_t Probe() {
        return g_probe.load(std::memory_order_relaxed);
    }

    template<typename F>
    long long MeasureNs(F&& fn) {
        auto begin = std::chrono::steady_clock::now(); // 微基准起点
        volatile std::uint64_t sink = fn();            // 保留可观察结果，避免被优化掉
        (void)sink;
        auto end = std::chrono::steady_clock::now();   // 微基准终点
        return std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count();
    }

    void Print(const char* name, long long likesNs, long long stdNs) {
        std::cout << name
            << " likes_ns=" << likesNs
            << " std_ns=" << stdNs
            << std::endl;
    }

    class NullSink : public LikesProgram::Log::Sink {
    public:
        NullSink() : Sink(u"NullSink") {}

        void Write(const LikesProgram::Log::Message& message) override {
            m_count.fetch_add(1, std::memory_order_relaxed);
            g_probe.fetch_add(static_cast<std::uint64_t>(message.msg.Length() & 1),
                std::memory_order_relaxed);
        }

        size_t Count() const {
            return m_count.load(std::memory_order_relaxed);
        }

    private:
        std::atomic<size_t> m_count{ 0 }; // 接收条数，用于保证异步分发真正发生
    };

    void ResetLogger(LikesProgram::Log::Logger& logger) {
        logger.Shutdown(true);
        logger.Configure(LikesProgram::Log::LoggerOptions());
        logger.SetLevel(LikesProgram::Log::Level::Trace);
        logger.SetEncoding(LikesProgram::String::Encoding::UTF8);
        logger.SetLoggerName(LikesProgram::String());
        LikesProgram::Log::Logger::ClearThreadName();
        LikesProgram::Log::Logger::ClearContext();
    }

    LP_BENCH_NOINLINE bool StdLevelAllowed(int level, int minimum) {
        return level >= minimum;
    }

    LP_BENCH_NOINLINE std::string StdFormatMessage(int index) {
        return std::string("value ") + std::to_string(index);
    }

    std::filesystem::path BenchmarkRootPath() {
        const auto now = std::chrono::steady_clock::now().time_since_epoch().count(); // 单次 benchmark 启动时间戳
#ifdef _WIN32
        const auto processId = static_cast<unsigned long>(_getpid()); // 当前进程 id，避免并行 benchmark 共用目录
#else
        const auto processId = static_cast<unsigned long>(getpid()); // 当前进程 id，避免并行 benchmark 共用目录
#endif
        return std::filesystem::temp_directory_path() /
            ("LikesProgramLoggingBenchmark-" + std::to_string(processId) + "-" + std::to_string(now));
    }

    void BenchmarkDisabledLevel(LikesProgram::Log::Logger& logger) {
        constexpr int iterations = 200000; // 过滤路径基准循环次数
        ResetLogger(logger);
        logger.SetLevel(LikesProgram::Log::Level::Warn);
        logger.AddSink(std::make_shared<NullSink>());
        logger.Start();

        auto likes = MeasureNs([&] {
            std::uint64_t total = 0;
            for (int i = 0; i < iterations; ++i) {
                logger.Log(LikesProgram::Log::Level::Debug, std::source_location::current(), u"disabled {}", i);
                total += static_cast<std::uint64_t>(i) + Probe();
            }
            return total;
        });
        logger.Shutdown(true);

        auto std = MeasureNs([&] {
            std::uint64_t total = 0;
            for (int i = 0; i < iterations; ++i) {
                if (StdLevelAllowed(1, 3)) total += static_cast<std::uint64_t>(i);
                total += Probe();
            }
            return total;
        });

        Print("disabled_level_filter", likes, std);
    }

    void BenchmarkEnabledNoArgs(LikesProgram::Log::Logger& logger) {
        constexpr int iterations = 50000; // 空格式化参数路径基准循环次数
        ResetLogger(logger);
        auto sink = std::make_shared<NullSink>();
        LikesProgram::Log::LoggerOptions options;
        options.maxQueueSize = 0;
        logger.Configure(options);
        logger.SetLevel(LikesProgram::Log::Level::Trace);
        logger.AddSink(sink);
        logger.Start();

        auto likes = MeasureNs([&] {
            std::uint64_t total = 0;
            for (int i = 0; i < iterations; ++i) {
                logger.Log(LikesProgram::Log::Level::Info, std::source_location::current(), u"static message");
                total += static_cast<std::uint64_t>(i) + Probe();
            }
            logger.Flush(std::chrono::seconds(10));
            return total + sink->Count();
        });
        logger.Shutdown(true);

        auto std = MeasureNs([&] {
            std::uint64_t total = 0;
            std::vector<std::string> queue;
            queue.reserve(iterations);
            for (int i = 0; i < iterations; ++i) {
                queue.emplace_back("static message");
                total += queue.back().size() + static_cast<std::uint64_t>(i) + Probe();
            }
            return total + queue.size();
        });

        Print("enabled_no_args_async_enqueue_flush_vs_std_vector", likes, std);
    }

    void BenchmarkEnabledFormat(LikesProgram::Log::Logger& logger) {
        constexpr int iterations = 30000; // 带格式化参数路径基准循环次数
        ResetLogger(logger);
        auto sink = std::make_shared<NullSink>();
        LikesProgram::Log::LoggerOptions options;
        options.maxQueueSize = 0;
        logger.Configure(options);
        logger.AddSink(sink);
        logger.Start();

        auto likes = MeasureNs([&] {
            std::uint64_t total = 0;
            for (int i = 0; i < iterations; ++i) {
                logger.Log(LikesProgram::Log::Level::Info, std::source_location::current(), u"value {}", i);
                total += static_cast<std::uint64_t>(i) + Probe();
            }
            logger.Flush(std::chrono::seconds(10));
            return total + sink->Count();
        });
        logger.Shutdown(true);

        auto std = MeasureNs([&] {
            std::uint64_t total = 0;
            std::vector<std::string> queue;
            queue.reserve(iterations);
            for (int i = 0; i < iterations; ++i) {
                queue.emplace_back(StdFormatMessage(i));
                total += queue.back().size() + Probe();
            }
            return total + queue.size();
        });

        Print("enabled_format_async_enqueue_flush_vs_std_string_build", likes, std);
    }

    void BenchmarkFileSink(LikesProgram::Log::Logger& logger) {
        constexpr int iterations = 3000; // 文件路径刻意保持较小，避免磁盘状态主导整轮基准
        auto root = BenchmarkRootPath(); // 本轮文件基准专属目录，避免并行运行相互清理
        std::filesystem::remove_all(root);

        ResetLogger(logger);
        LikesProgram::Log::FileSinkOptions options;
        options.maxFileSizeMB = 0;
        options.maxFileSizeBytes = 0;
        logger.AddSink(LikesProgram::Log::FileSink::CreateSink(
            LikesProgram::String(root.string()), u"benchmark.log", options));
        logger.Start();

        auto likes = MeasureNs([&] {
            std::uint64_t total = 0;
            for (int i = 0; i < iterations; ++i) {
                logger.Log(LikesProgram::Log::Level::Info, std::source_location::current(), u"file {}", i);
                total += static_cast<std::uint64_t>(i) + Probe();
            }
            logger.Flush(std::chrono::seconds(10));
            return total;
        });
        logger.Shutdown(true);

        const auto stdPath = root / "std-benchmark.log";
        auto std = MeasureNs([&] {
            std::uint64_t total = 0;
            std::ofstream file(stdPath, std::ios::binary | std::ios::trunc);
            for (int i = 0; i < iterations; ++i) {
                auto line = StdFormatMessage(i);
                file << line << '\n';
                total += line.size() + Probe();
            }
            file.flush();
            return total;
        });

        std::filesystem::remove_all(root);
        Print("file_sink_async_flush_vs_std_ofstream", likes, std);
    }

    void BenchmarkMultiThread(LikesProgram::Log::Logger& logger) {
        constexpr int threadCount = 4; // 多生产者基准覆盖普通服务并发写入规模
        constexpr int perThread = 10000;

        ResetLogger(logger);
        auto sink = std::make_shared<NullSink>();
        LikesProgram::Log::LoggerOptions options;
        options.maxQueueSize = 0;
        logger.Configure(options);
        logger.AddSink(sink);
        logger.Start();

        auto likes = MeasureNs([&] {
            std::vector<std::thread> threads;
            for (int t = 0; t < threadCount; ++t) {
                threads.emplace_back([&logger, t] {
                    for (int i = 0; i < perThread; ++i) {
                        logger.Log(LikesProgram::Log::Level::Info, std::source_location::current(),
                            u"thread {} item {}", t, i);
                    }
                });
            }
            for (auto& thread : threads) thread.join();
            logger.Flush(std::chrono::seconds(10));
            return static_cast<std::uint64_t>(sink->Count()) + Probe();
        });
        logger.Shutdown(true);

        auto std = MeasureNs([&] {
            std::uint64_t total = 0;
            std::mutex mutex;
            std::vector<std::string> queue;
            queue.reserve(threadCount * perThread);
            std::vector<std::thread> threads;
            for (int t = 0; t < threadCount; ++t) {
                threads.emplace_back([&queue, &mutex, &total, t] {
                    for (int i = 0; i < perThread; ++i) {
                        auto line = std::string("thread ") + std::to_string(t) +
                            " item " + std::to_string(i);
                        std::lock_guard<std::mutex> lock(mutex);
                        total += line.size() + Probe();
                        queue.push_back(std::move(line));
                    }
                });
            }
            for (auto& thread : threads) thread.join();
            return total + queue.size();
        });

        Print("multi_thread_async_enqueue_flush_vs_std_mutex_vector", likes, std);
    }
}

int main() {
    auto& logger = LikesProgram::Log::Logger::Instance(false, false);

    BenchmarkDisabledLevel(logger);
    BenchmarkEnabledNoArgs(logger);
    BenchmarkEnabledFormat(logger);
    BenchmarkFileSink(logger);
    BenchmarkMultiThread(logger);

    logger.Shutdown(true);
    return 0;
}
