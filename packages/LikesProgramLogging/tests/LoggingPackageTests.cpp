#include <LikesProgram/Logging/Logging.hpp>
#include <LikesProgram/Core/Version.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace {
    class CountingSink : public LikesProgram::Log::Sink {
    public:
        CountingSink() : Sink(u"CountingSink") {}

        void Write(const LikesProgram::Log::Message&) override {
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_count;
        }

        size_t Count() const {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_count;
        }

    private:
        mutable std::mutex m_mutex; // 保护计数，测试中后台线程和断言线程并发访问
        size_t m_count = 0;         // 已接收到的日志条数
    };

    class ThrowingSink : public LikesProgram::Log::Sink {
    public:
        ThrowingSink() : Sink(u"ThrowingSink") {}

        void Write(const LikesProgram::Log::Message&) override {
            throw std::runtime_error("intentional sink failure");
        }
    };

    class FlushThrowingSink : public LikesProgram::Log::Sink {
    public:
        FlushThrowingSink() : Sink(u"FlushThrowingSink") {}

        void Write(const LikesProgram::Log::Message&) override {
            // 写入保持成功，专门验证 Flush 异常不会破坏 Logger。
        }

        void Flush() override {
            throw std::runtime_error("intentional flush failure");
        }
    };

    class FlakySink : public LikesProgram::Log::Sink {
    public:
        explicit FlakySink(size_t failuresBeforeSuccess)
            : Sink(u"FlakySink"), m_remainingFailures(failuresBeforeSuccess) {
        }

        void Write(const LikesProgram::Log::Message&) override {
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_attempts;
            if (m_remainingFailures > 0) {
                --m_remainingFailures;
                throw std::runtime_error("intentional flaky sink failure");
            }
            ++m_successes;
        }

        size_t Attempts() const {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_attempts;
        }

        size_t Successes() const {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_successes;
        }

    private:
        mutable std::mutex m_mutex;      // 保护失败计数和成功计数
        size_t m_remainingFailures = 0;  // 还需要抛出的失败次数
        size_t m_attempts = 0;           // Write 被调用次数
        size_t m_successes = 0;          // 成功写入次数
    };

    class AlwaysFailingSink : public LikesProgram::Log::Sink {
    public:
        AlwaysFailingSink() : Sink(u"AlwaysFailingSink") {}

        void Write(const LikesProgram::Log::Message&) override {
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_attempts;
            m_cv.notify_all();
            throw std::runtime_error("intentional always failing sink");
        }

        size_t Attempts() const {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_attempts;
        }

        bool WaitAttempts(size_t expected, std::chrono::milliseconds timeout) {
            std::unique_lock<std::mutex> lock(m_mutex);
            return m_cv.wait_for(lock, timeout, [this, expected] { return m_attempts >= expected; });
        }

    private:
        mutable std::mutex m_mutex; // 保护失败计数，后台线程和断言线程并发访问
        std::condition_variable m_cv;
        size_t m_attempts = 0;      // Write 被调用次数
    };

    class BlockingSink : public LikesProgram::Log::Sink {
    public:
        BlockingSink() : Sink(u"BlockingSink") {}

        void Write(const LikesProgram::Log::Message&) override {
            std::unique_lock<std::mutex> lock(m_mutex);
            ++m_seen;
            m_started = true;
            m_startedCv.notify_all();
            m_releaseCv.wait(lock, [this] { return m_released; });
        }

        bool WaitStarted(std::chrono::milliseconds timeout) {
            std::unique_lock<std::mutex> lock(m_mutex);
            return m_startedCv.wait_for(lock, timeout, [this] { return m_started; });
        }

        void Release() {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_released = true;
            m_releaseCv.notify_all();
        }

    private:
        std::mutex m_mutex;                         // 保护阻塞状态，测试线程和日志线程并发访问
        std::condition_variable m_startedCv;        // 通知测试线程第一条日志已进入 Sink
        std::condition_variable m_releaseCv;        // 通知 Sink 解除阻塞
        size_t m_seen = 0;                          // 已进入 Sink 的消息数
        bool m_started = false;                     // 第一条日志是否已到达 Sink
        bool m_released = false;                    // 是否允许 Sink 返回
    };

    class CapturingSink : public LikesProgram::Log::Sink {
    public:
        CapturingSink() : Sink(u"CapturingSink") {}

        void Write(const LikesProgram::Log::Message& message) override {
            LikesProgram::String formatted = FormatLogMessage(message); // 复用真实 Sink 格式化路径
            std::lock_guard<std::mutex> lock(m_mutex);
            m_lines.push_back(std::move(formatted));
        }

        std::vector<LikesProgram::String> Lines() const {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_lines;
        }

    private:
        mutable std::mutex m_mutex;                  // 保护捕获日志，后台线程和断言线程并发访问
        std::vector<LikesProgram::String> m_lines;   // 已格式化的日志文本快照
    };

    struct ThrowOnCopy {
        ThrowOnCopy() = default;

        ThrowOnCopy(const ThrowOnCopy&) {
            throw std::runtime_error("disabled level should not format arguments");
        }

        ThrowOnCopy& operator=(const ThrowOnCopy&) = delete;
    };

    void Require(bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    }

    std::filesystem::path MakeTempRoot(const char* testName) {
        std::ostringstream name;
        name << testName << "-" << std::this_thread::get_id();
        return std::filesystem::temp_directory_path() / name.str();
    }

    bool IsManagedFileName(const std::filesystem::path& path, const std::string& baseName) {
        std::string name = path.filename().string();
        if (name == baseName) return true;
        if (name.size() <= baseName.size() + 1) return false;
        if (name.substr(name.size() - baseName.size()) != baseName) return false;
        if (name[name.size() - baseName.size() - 1] != '_') return false;

        const size_t prefixSize = name.size() - baseName.size() - 1; // 轮转索引前缀长度
        for (size_t i = 0; i < prefixSize; ++i) {
            if (name[i] < '0' || name[i] > '9') return false;
        }
        return true;
    }

    void WriteFileForRetentionTest(const std::filesystem::path& path, const std::string& content,
        std::filesystem::file_time_type writeTime) {
        std::filesystem::create_directories(path.parent_path());

        std::ofstream file(path, std::ios::binary); // 明确写入测试内容大小，方便总大小清理断言
        file << content;
        file.close();

        std::filesystem::last_write_time(path, writeTime);
    }

    size_t CountManagedFiles(const std::filesystem::path& root, const std::string& baseName) {
        if (!std::filesystem::exists(root)) return 0;

        size_t count = 0; // 当前根目录下符合 FileSink 命名规则的文件数
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
            if (entry.is_regular_file() && IsManagedFileName(entry.path(), baseName)) ++count;
        }
        return count;
    }

    void ResetLogger(LikesProgram::Log::Logger& logger) {
        logger.Shutdown(true);
        logger.Configure(LikesProgram::Log::LoggerOptions());
        logger.SetLevel(LikesProgram::Log::Level::Trace);
        logger.SetEncoding(LikesProgram::String::Encoding::UTF8);
        logger.SetLoggerName(LikesProgram::String());
        LikesProgram::Log::Logger::ClearThreadName();
        LikesProgram::Log::Logger::ClearContext();
    }

    void TestPackageIdentity() {
        const char* packageName = LikesProgram::Logging::PackageName();
        const char* packageVersion = LikesProgram::Logging::PackageVersion();

        Require(LikesProgram::Logging::PackageAvailable(), "Logging package should be available");
        Require(std::strcmp(packageName, "LikesProgramLogging") == 0, "Logging package name mismatch");
        Require(std::strcmp(packageVersion, LikesProgram::Version::CurrentString().data()) == 0,
            "Logging package version should follow Core version");
        LikesProgram::Logger* loggerAlias = &LikesProgram::Log::Logger::Instance(); // 顶层短别名应指向旧 Log 命名空间主入口
        Require(loggerAlias == &LikesProgram::Log::Logger::Instance(), "Logger alias should match Log::Logger");
    }

    void TestLevelConversion() {
        Require(LikesProgram::Log::LevelToString(LikesProgram::Log::Level::Warn) == u"Warn",
            "LevelToString should keep old spelling");
        Require(LikesProgram::Log::StringToLevel(u"error") == LikesProgram::Log::Level::Error,
            "StringToLevel should parse lowercase text");
        Require(LikesProgram::Log::StringToLevel(u"missing", LikesProgram::Log::Level::Fatal) ==
            LikesProgram::Log::Level::Fatal, "StringToLevel should return default for unknown text");
    }

    void TestDisabledLevelSkipsFormatting() {
        auto& logger = LikesProgram::Log::Logger::Instance(false, false);
        ResetLogger(logger);

        auto sink = std::make_shared<CountingSink>();
        ThrowOnCopy value;
        logger.SetLevel(LikesProgram::Log::Level::Warn);
        logger.AddSink(sink);

        Require(logger.Start(), "Logger should start for disabled level test");
        logger.Log(LikesProgram::Log::Level::Debug, std::source_location::current(), u"{}", value);
        Require(logger.Flush(std::chrono::seconds(5)), "Flush should complete after disabled log");
        Require(sink->Count() == 0, "Disabled log should not reach sinks");
        logger.Shutdown(true);
    }

    void TestLoggerFileSink() {
        auto root = MakeTempRoot("LikesProgramLoggingTests");
        std::filesystem::remove_all(root);

        auto& logger = LikesProgram::Log::Logger::Instance(false, true);
        ResetLogger(logger);
        logger.AddSink(LikesProgram::Log::FileSink::CreateSink(LikesProgram::String(root.string()),
            u"logging-test.log", 1));

        Require(logger.Start(), "Logger should start with FileSink");
        logger.Log(LikesProgram::Log::Level::Info, std::source_location::current(),
            u"hello {}", u"logging");
        Require(logger.Flush(std::chrono::seconds(5)), "Flush should drain FileSink");
        logger.Shutdown(true);

        bool found = false;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
            if (entry.is_regular_file() && entry.path().filename() == "logging-test.log") {
                found = true;
                Require(entry.file_size() > 0, "FileSink should write non-empty log file");
            }
        }

        std::filesystem::remove_all(root);
        Require(found, "FileSink should create expected log file");
    }

    void TestFileSinkOpenFailureBoundary() {
        auto root = MakeTempRoot("LikesProgramLoggingFileSinkFailure");
        std::filesystem::remove_all(root);

        {
            std::ofstream blocker(root, std::ios::binary); // 用普通文件占住日志根路径，制造目录创建失败
            blocker << "not a directory";
        }

        bool threw = false;
        try {
            LikesProgram::Log::FileSinkOptions options;
            LikesProgram::Log::FileSink sink(LikesProgram::String(root.string()), u"blocked.log", options);
        }
        catch (const std::exception&) {
            threw = true;
        }

        std::filesystem::remove(root);
        Require(threw, "FileSink should surface open failure when log root is not a directory");
    }

    void TestConsoleSinkWriteBoundary() {
        LikesProgram::Log::ConsoleSink sink;

        LikesProgram::Log::Message message;
        message.level = LikesProgram::Log::Level::Warn;
        message.msg = u"console boundary";
        message.tid = std::this_thread::get_id();
        message.timestamp = std::chrono::system_clock::now();
        message.encoding = LikesProgram::String::Encoding::UTF8;
        message.outputFormat = LikesProgram::Log::LogOutputFormat::Text;
        message.processId = 1;

        sink.Write(message);
        message.outputFormat = LikesProgram::Log::LogOutputFormat::JsonLines;
        sink.Write(message);
        sink.Flush();
    }

    void TestFileSinkRetentionPolicy() {
        auto root = MakeTempRoot("LikesProgramLoggingRetentionTests");
        std::filesystem::remove_all(root);

        const auto oldDir = root / "2000-01-01";
        const auto now = std::filesystem::file_time_type::clock::now();
        WriteFileForRetentionTest(oldDir / "retention.log", "old-a", now - std::chrono::hours(5));
        WriteFileForRetentionTest(oldDir / "1_retention.log", "old-b", now - std::chrono::hours(4));
        WriteFileForRetentionTest(oldDir / "2_retention.log", "old-c", now - std::chrono::hours(3));
        WriteFileForRetentionTest(oldDir / "foreign.log", "keep", now - std::chrono::hours(6));

        {
            LikesProgram::Log::FileSinkOptions options;
            options.maxRetainedFiles = 2;
            LikesProgram::Log::FileSink sink(LikesProgram::String(root.string()), u"retention.log", options);

            Require(CountManagedFiles(root, "retention.log") == 2,
                "FileSink should keep only newest managed files plus current file");
            Require(std::filesystem::exists(oldDir / "foreign.log"),
                "FileSink retention should not delete unmanaged file names");

            auto runtimeOptions = sink.Options();
            runtimeOptions.maxRetainedFiles = 0;
            runtimeOptions.retentionDays = 1;
            const auto stale = oldDir / "3_retention.log";
            WriteFileForRetentionTest(stale, "stale", now - std::chrono::hours(48));
            sink.Configure(runtimeOptions);
            Require(!std::filesystem::exists(stale),
                "FileSink Configure should apply retention days at runtime");

            runtimeOptions.retentionDays = 0;
            runtimeOptions.maxTotalSizeBytes = 1;
            const auto large = oldDir / "4_retention.log";
            WriteFileForRetentionTest(large, std::string(32, 'x'), now - std::chrono::hours(1));
            sink.Configure(runtimeOptions);
            Require(!std::filesystem::exists(large),
                "FileSink Configure should apply total size retention at runtime");
            Require(CountManagedFiles(root, "retention.log") == 1,
                "FileSink retention should keep the currently open file");
        }

        std::filesystem::remove_all(root);
    }

    void TestLoggerFlushAndStats() {
        auto& logger = LikesProgram::Log::Logger::Instance(false, false);
        ResetLogger(logger);

        auto sink = std::make_shared<CountingSink>();
        logger.AddSink(sink);
        Require(logger.Start(), "Logger should start with CountingSink");

        constexpr int threadCount = 4;
        constexpr int perThread = 250;
        std::vector<std::thread> threads;
        for (int t = 0; t < threadCount; ++t) {
            threads.emplace_back([&logger, t] {
                for (int i = 0; i < perThread; ++i) {
                    logger.Log(LikesProgram::Log::Level::Info, std::source_location::current(),
                        u"thread {} message {}", t, i);
                }
            });
        }

        for (auto& thread : threads) thread.join();

        Require(logger.Flush(std::chrono::seconds(5)), "Flush should drain concurrent logs");
        auto stats = logger.Stats();
        Require(sink->Count() == static_cast<size_t>(threadCount * perThread),
            "CountingSink should receive all concurrent logs");
        Require(stats.acceptedMessages >= static_cast<uint64_t>(threadCount * perThread),
            "Stats should count accepted messages");
        Require(stats.processedMessages >= static_cast<uint64_t>(threadCount * perThread),
            "Stats should count processed messages");
        Require(stats.currentQueueSize == 0, "Queue should be empty after Flush");
        logger.Shutdown(true);
    }

    void TestSinkFailureIsolation() {
        auto& logger = LikesProgram::Log::Logger::Instance(false, false);
        ResetLogger(logger);

        auto counting = std::make_shared<CountingSink>();
        logger.AddSink(std::make_shared<ThrowingSink>());
        logger.AddSink(counting);

        Require(logger.Start(), "Logger should start with throwing sink");
        logger.Log(LikesProgram::Log::Level::Error, std::source_location::current(), u"after failure");
        Require(logger.Flush(std::chrono::seconds(5)), "Flush should complete despite failing sink");

        auto stats = logger.Stats();
        Require(counting->Count() == 1, "Healthy sink should still receive message");
        Require(stats.sinkWriteFailures >= 1, "Sink failure should be counted");
        logger.Shutdown(true);
    }

    void TestQueueBackpressureDropNewest() {
        auto& logger = LikesProgram::Log::Logger::Instance(false, false);
        ResetLogger(logger);

        auto blocking = std::make_shared<BlockingSink>();
        LikesProgram::Log::LoggerOptions options;
        options.maxQueueSize = 1;
        options.overflowPolicy = LikesProgram::Log::QueueOverflowPolicy::DropNewest;
        logger.Configure(options);
        logger.AddSink(blocking);

        const auto before = logger.Stats();
        Require(logger.Start(), "Logger should start with bounded queue");
        for (int i = 0; i < 512; ++i) {
            logger.Log(LikesProgram::Log::Level::Info, std::source_location::current(), u"drop {}", i);
            if (i == 0) Require(blocking->WaitStarted(std::chrono::seconds(5)),
                "BlockingSink should receive first message");
        }

        blocking->Release();
        Require(logger.Flush(std::chrono::seconds(5)), "Flush should drain bounded queue");

        auto stats = logger.Stats();
        Require(stats.droppedMessages > before.droppedMessages, "Bounded queue should drop newest messages");
        Require(stats.currentQueueSize == 0, "Bounded queue should be empty after Flush");
        logger.Shutdown(true);
    }

    void TestQueueBackpressureDropOldest() {
        auto& logger = LikesProgram::Log::Logger::Instance(false, false);
        ResetLogger(logger);

        auto blocking = std::make_shared<BlockingSink>();
        LikesProgram::Log::LoggerOptions options;
        options.maxQueueSize = 1;
        options.overflowPolicy = LikesProgram::Log::QueueOverflowPolicy::DropOldest;
        logger.Configure(options);
        logger.AddSink(blocking);

        const auto before = logger.Stats();
        Require(logger.Start(), "Logger should start with drop oldest queue");
        for (int i = 0; i < 512; ++i) {
            logger.Log(LikesProgram::Log::Level::Info, std::source_location::current(), u"drop-oldest {}", i);
            if (i == 0) Require(blocking->WaitStarted(std::chrono::seconds(5)),
                "BlockingSink should receive first drop oldest message");
        }

        blocking->Release();
        Require(logger.Flush(std::chrono::seconds(5)), "Flush should drain drop oldest queue");

        auto stats = logger.Stats();
        Require(stats.droppedMessages > before.droppedMessages, "DropOldest should drop older messages");
        Require(stats.currentQueueSize == 0, "DropOldest queue should be empty after Flush");
        logger.Shutdown(true);
    }

    void TestShutdownTimeoutRecovery() {
        auto& logger = LikesProgram::Log::Logger::Instance(false, false);
        ResetLogger(logger);

        auto blocking = std::make_shared<BlockingSink>();
        logger.AddSink(blocking);

        Require(logger.Start(), "Logger should start for shutdown timeout recovery");
        logger.Log(LikesProgram::Log::Level::Info, std::source_location::current(), u"blocked shutdown");
        Require(blocking->WaitStarted(std::chrono::seconds(5)),
            "BlockingSink should receive shutdown timeout message");

        Require(!logger.Shutdown(std::chrono::milliseconds(10), true),
            "Shutdown with short timeout should report timeout");
        Require(logger.Stats().flushTimeouts >= 1, "Shutdown timeout should be counted");

        blocking->Release();
        Require(logger.Shutdown(std::chrono::seconds(5), true),
            "Later Shutdown should join worker after blocked sink is released");
        Require(logger.Stats().sinkCount == 0, "Recovered shutdown should clear sinks");
    }

    void TestSinkFlushFailureIsolation() {
        auto& logger = LikesProgram::Log::Logger::Instance(false, false);
        ResetLogger(logger);

        auto counting = std::make_shared<CountingSink>();
        logger.AddSink(std::make_shared<FlushThrowingSink>());
        logger.AddSink(counting);

        Require(logger.Start(), "Logger should start with flush throwing sink");
        logger.Log(LikesProgram::Log::Level::Info, std::source_location::current(), u"flush failure");
        Require(logger.Flush(std::chrono::seconds(5)), "Flush should return true even if a sink flush fails");

        auto stats = logger.Stats();
        Require(counting->Count() == 1, "Healthy sink should receive message when another Flush fails");
        Require(stats.sinkWriteFailures >= 1, "Flush failure should be counted");
        logger.Shutdown(true);
    }

    void TestLoggerConfigOpenSinkAndRetry() {
        auto& logger = LikesProgram::Log::Logger::Instance(false, false);
        ResetLogger(logger);

        auto flaky = std::make_shared<FlakySink>(1);
        LikesProgram::Log::LoggerConfig config;
        config.level = LikesProgram::Log::Level::Trace;
        config.loggerName = u"config-logger";
        config.debug = true;
        config.options.maxQueueSize = 0;

        LikesProgram::Log::SinkConfig sinkConfig;
        sinkConfig.name = u"custom-flaky";
        sinkConfig.minLevel = LikesProgram::Log::Level::Warn;
        sinkConfig.retry.enabled = true;
        sinkConfig.retry.maxAttempts = 2;
        sinkConfig.sink = flaky;
        config.sinks.push_back(sinkConfig);

        auto status = logger.ApplyConfig(config);
        Require(status.IsOk(), "LoggerConfig should apply open custom sink config");
        Require(logger.Start(), "Logger should start with LoggerConfig");

        logger.Log(LikesProgram::Log::Level::Info, std::source_location::current(), u"below sink level");
        logger.Log(LikesProgram::Log::Level::Error, std::source_location::current(), u"retry once");
        Require(logger.Flush(std::chrono::seconds(5)), "Flush should wait retry queue");

        auto stats = logger.Stats();
        Require(flaky->Attempts() >= 2, "Retry should call flaky sink again after first failure");
        Require(flaky->Successes() == 1, "Retry should eventually deliver message to flaky sink");
        Require(stats.sinkRetryScheduled >= 1, "Stats should expose scheduled sink retries");

        auto effective = logger.EffectiveConfig();
        Require(effective.IsOk(), "EffectiveConfig should return applied config snapshot");
        Require(effective.Value().loggerName == u"config-logger",
            "EffectiveConfig should include logger name");
        Require(effective.Value().sinks.size() == 1,
            "EffectiveConfig should include configured sink");
        Require(effective.Value().sinks[0].name == u"custom-flaky",
            "EffectiveConfig should preserve open sink logical name");

        logger.Shutdown(true);
    }

    void TestLoggerRetryQueueBounded() {
        auto& logger = LikesProgram::Log::Logger::Instance(false, false);
        ResetLogger(logger);

        auto failing = std::make_shared<AlwaysFailingSink>();
        LikesProgram::Log::LoggerConfig config;
        config.level = LikesProgram::Log::Level::Trace;
        config.options.maxQueueSize = 0;

        LikesProgram::Log::SinkConfig sinkConfig;
        sinkConfig.name = u"bounded-retry";
        sinkConfig.retry.enabled = true;
        sinkConfig.retry.maxAttempts = 3;
        sinkConfig.retry.maxQueueSize = 1;
        sinkConfig.retry.initialBackoff = std::chrono::milliseconds(50);
        sinkConfig.sink = failing;
        config.sinks.push_back(sinkConfig);

        Require(logger.ApplyConfig(config).IsOk(), "LoggerConfig should accept bounded retry queue");
        Require(logger.Start(), "Logger should start for bounded retry queue test");

        for (int i = 0; i < 16; ++i) {
            logger.Log(LikesProgram::Log::Level::Error, std::source_location::current(), u"bounded retry {}", i);
        }

        Require(logger.Flush(std::chrono::seconds(5)), "Flush should drain bounded retry queue");
        auto stats = logger.Stats();
        Require(stats.retryQueueHighWatermark <= sinkConfig.retry.maxQueueSize,
            "Bounded retry queue should keep retry queue high watermark within configured limit");
        Require(stats.sinkRetryDropped > 0,
            "Bounded retry queue should count retries dropped by queue limit or exhaustion");
        Require(stats.sinkRetryScheduled >= 1,
            "Bounded retry queue should still schedule allowed retries");
        Require(failing->Attempts() >= 16,
            "Failing sink should receive original write attempts before retry bounding");

        logger.Shutdown(true);
    }

    void TestLoggerFlushUsesSingleTimeoutBudgetForRetryDrain() {
        auto& logger = LikesProgram::Log::Logger::Instance(false, false);
        ResetLogger(logger);

        auto failing = std::make_shared<AlwaysFailingSink>();
        LikesProgram::Log::LoggerConfig config;
        config.level = LikesProgram::Log::Level::Trace;
        config.options.maxQueueSize = 0;

        LikesProgram::Log::SinkConfig sinkConfig;
        sinkConfig.name = u"slow-retry";
        sinkConfig.retry.enabled = true;
        sinkConfig.retry.maxAttempts = 1;
        sinkConfig.retry.maxQueueSize = 1;
        sinkConfig.retry.initialBackoff = std::chrono::seconds(2);
        sinkConfig.sink = failing;
        config.sinks.push_back(sinkConfig);

        Require(logger.ApplyConfig(config).IsOk(), "LoggerConfig should accept slow retry test config");
        Require(logger.Start(), "Logger should start for retry timeout budget test");
        logger.Log(LikesProgram::Log::Level::Error, std::source_location::current(), u"slow retry");

        const auto begin = std::chrono::steady_clock::now();
        Require(!logger.Flush(std::chrono::milliseconds(100)),
            "Flush should fail when retry drain exceeds the same timeout budget");
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - begin);
        Require(elapsed < std::chrono::seconds(1),
            "Flush should not spend a second full timeout on retry drain");
        Require(logger.Stats().flushTimeouts >= 1,
            "Flush timeout caused by retry drain should be counted");

        logger.Shutdown(std::chrono::milliseconds::max(), true);
    }

    void TestLoggerShutdownStopsRetryExpansion() {
        auto& logger = LikesProgram::Log::Logger::Instance(false, false);
        ResetLogger(logger);

        auto failing = std::make_shared<AlwaysFailingSink>();
        LikesProgram::Log::LoggerConfig config;
        config.level = LikesProgram::Log::Level::Trace;
        config.options.maxQueueSize = 0;

        LikesProgram::Log::SinkConfig sinkConfig;
        sinkConfig.name = u"shutdown-retry";
        sinkConfig.retry.enabled = true;
        sinkConfig.retry.maxAttempts = 100000;
        sinkConfig.retry.maxQueueSize = 1;
        sinkConfig.retry.initialBackoff = std::chrono::hours(1);
        sinkConfig.sink = failing;
        config.sinks.push_back(sinkConfig);

        Require(logger.ApplyConfig(config).IsOk(), "LoggerConfig should accept shutdown retry test config");
        Require(logger.Start(), "Logger should start for shutdown retry expansion test");
        logger.Log(LikesProgram::Log::Level::Error, std::source_location::current(), u"shutdown retry");
        Require(failing->WaitAttempts(1, std::chrono::seconds(5)),
            "Shutdown retry expansion test should observe the original failing write");

        const auto begin = std::chrono::steady_clock::now();
        Require(logger.Shutdown(std::chrono::seconds(5), true),
            "Shutdown should not expand retry chain after stop");
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - begin);
        Require(elapsed < std::chrono::seconds(1),
            "Shutdown should not spend large retry backoff or maxAttempts after stop");
        Require(failing->Attempts() <= 2,
            "Shutdown should complete at most the in-flight retry after stop");
    }

    void TestLoggerConfigValidation() {
        LikesProgram::Log::LoggerConfig config;
        LikesProgram::Log::SinkConfig sinkConfig;
        sinkConfig.name = u"bad-retry";
        sinkConfig.retry.enabled = true;
        sinkConfig.retry.maxAttempts = 0;
        sinkConfig.sink = std::make_shared<CountingSink>();
        config.sinks.push_back(sinkConfig);

        auto status = LikesProgram::Log::ValidateLoggerConfig(config);
        Require(!status.IsOk(), "LoggerConfig validation should reject enabled retry without attempts");

        sinkConfig.retry.maxAttempts = 1;
        sinkConfig.retry.maxQueueSize = 0;
        config.sinks.clear();
        config.sinks.push_back(sinkConfig);
        status = LikesProgram::Log::ValidateLoggerConfig(config);
        Require(!status.IsOk(), "LoggerConfig validation should reject enabled retry without queue bound");

        config.sinks.clear();
        config.options.overflowPolicy = static_cast<LikesProgram::Log::QueueOverflowPolicy>(99);
        status = LikesProgram::Log::ValidateLoggerConfig(config);
        Require(!status.IsOk(), "LoggerConfig validation should reject invalid overflow policy");
    }

    void TestLoggerConfigureNormalizesInvalidRuntimeOptions() {
        auto& logger = LikesProgram::Log::Logger::Instance(false, false);
        ResetLogger(logger);

        LikesProgram::Log::LoggerOptions options;
        options.overflowPolicy = static_cast<LikesProgram::Log::QueueOverflowPolicy>(99);
        options.outputFormat = static_cast<LikesProgram::Log::LogOutputFormat>(99);
        logger.Configure(options);

        auto effective = logger.Options();
        Require(effective.overflowPolicy == LikesProgram::Log::QueueOverflowPolicy::Block,
            "Logger::Configure should normalize invalid overflow policy to Block");
        Require(effective.outputFormat == LikesProgram::Log::LogOutputFormat::Text,
            "Logger::Configure should normalize invalid output format to Text");
    }

    void TestFileSinkMultiProcessConfig() {
        auto root = MakeTempRoot("LikesProgramLoggingMultiProcessFileTests");
        std::filesystem::remove_all(root);

        {
            auto& logger = LikesProgram::Log::Logger::Instance(false, false);
            ResetLogger(logger);

            LikesProgram::Log::FileSinkConfig fileConfig;
            fileConfig.name = u"mp-file";
            fileConfig.path = LikesProgram::String(root.string());
            fileConfig.filename = u"multi-process.log";
            fileConfig.multiProcess.enabled = true;
            fileConfig.multiProcess.lockName = u"LikesProgramLoggingTestsMultiProcessFile";
            fileConfig.multiProcess.lockTimeout = std::chrono::seconds(1);

            auto sinkConfig = LikesProgram::Log::MakeFileSinkConfig(fileConfig);
            Require(sinkConfig.IsOk(), "MakeFileSinkConfig should create multi-process FileSink");

            LikesProgram::Log::LoggerConfig config;
            config.sinks.push_back(sinkConfig.Value());
            Require(logger.ApplyConfig(config).IsOk(), "LoggerConfig should apply FileSink config");
            Require(logger.Start(), "Logger should start with multi-process FileSink config");

            logger.Log(LikesProgram::Log::Level::Info, std::source_location::current(), u"multi process file");
            Require(logger.Flush(std::chrono::seconds(5)), "Flush should drain multi-process FileSink");
            logger.Shutdown(true);
        }

        bool found = false;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
            if (entry.is_regular_file() && entry.path().filename() == "multi-process.log") {
                found = true;
                Require(entry.file_size() > 0, "Multi-process FileSink should write non-empty file");
            }
        }

        std::filesystem::remove_all(root);
        Require(found, "Multi-process FileSink should create expected log file");
    }

    void TestFileSinkMultiProcessRotationSeesPeerWrites() {
        auto root = MakeTempRoot("LikesProgramLoggingMultiProcessRotationTests");
        std::filesystem::remove_all(root);

        LikesProgram::Log::FileSinkOptions options;
        options.maxFileSizeBytes = 120;

        LikesProgram::Log::MultiProcessFileConfig multiProcess;
        multiProcess.enabled = true;
        multiProcess.lockName = u"LikesProgramLoggingTestsMultiProcessRotation";
        multiProcess.lockTimeout = std::chrono::seconds(1);

        {
            LikesProgram::Log::FileSink first(LikesProgram::String(root.string()),
                u"rotation.log", options, multiProcess);
            LikesProgram::Log::FileSink second(LikesProgram::String(root.string()),
                u"rotation.log", options, multiProcess);

            LikesProgram::Log::Message message;
            message.level = LikesProgram::Log::Level::Info;
            message.tid = std::this_thread::get_id();
            message.timestamp = std::chrono::system_clock::now();
            message.encoding = LikesProgram::String::Encoding::UTF8;
            message.outputFormat = LikesProgram::Log::LogOutputFormat::Text;
            message.processId = 1;
            message.msg = LikesProgram::String(std::string(96, 'x'));

            first.Write(message);
            first.Flush();
            second.Write(message);
            second.Flush();
        }

        Require(CountManagedFiles(root, "rotation.log") >= 2,
            "Multi-process FileSink should rotate after seeing peer file size");
        std::filesystem::remove_all(root);
    }

    void TestLoggerStress100k() {
        auto& logger = LikesProgram::Log::Logger::Instance(false, false);
        ResetLogger(logger);

        auto sink = std::make_shared<CountingSink>();
        LikesProgram::Log::LoggerOptions options;
        options.maxQueueSize = 0;
        logger.Configure(options);
        logger.AddSink(sink);

        constexpr int threadCount = 8;
        constexpr int perThread = 12500;
        Require(logger.Start(), "Logger should start for 100k stress test");

        std::vector<std::thread> threads;
        for (int t = 0; t < threadCount; ++t) {
            threads.emplace_back([&logger, t] {
                for (int i = 0; i < perThread; ++i) {
                    logger.Log(LikesProgram::Log::Level::Info, std::source_location::current(),
                        u"stress {} {}", t, i);
                }
            });
        }

        for (auto& thread : threads) thread.join();
        Require(logger.Flush(std::chrono::seconds(10)), "Flush should drain 100k stress logs");

        auto stats = logger.Stats();
        const auto expected = static_cast<size_t>(threadCount * perThread);
        Require(sink->Count() == expected, "Stress sink should receive all 100k logs");
        Require(stats.currentQueueSize == 0, "Stress queue should be empty after Flush");
        Require(stats.acceptedMessages >= expected, "Stress stats should include accepted logs");
        Require(stats.processedMessages >= expected, "Stress stats should include processed logs");
        logger.Shutdown(true);
    }

    void TestRuntimeSinkReplacement() {
        auto& logger = LikesProgram::Log::Logger::Instance(false, false);
        ResetLogger(logger);

        auto first = std::make_shared<CountingSink>();
        auto second = std::make_shared<CountingSink>();
        logger.AddSink(first);

        Require(logger.Start(), "Logger should start for sink replacement test");
        logger.Log(LikesProgram::Log::Level::Info, std::source_location::current(), u"first sink");
        Require(logger.Flush(std::chrono::seconds(5)), "Flush should drain first sink message");
        Require(first->Count() == 1, "First sink should receive initial message");
        Require(second->Count() == 0, "Second sink should not receive before replacement");

        logger.SetSinks({ nullptr, second });
        Require(logger.Stats().sinkCount == 1, "SetSinks should filter null sink entries");
        logger.Log(LikesProgram::Log::Level::Info, std::source_location::current(), u"second sink");
        Require(logger.Flush(std::chrono::seconds(5)), "Flush should drain replacement sink message");
        Require(first->Count() == 1, "First sink should stop receiving after replacement");
        Require(second->Count() == 1, "Second sink should receive after replacement");

        logger.ClearSinks();
        Require(logger.Stats().sinkCount == 0, "ClearSinks should remove all sinks");
        logger.Log(LikesProgram::Log::Level::Info, std::source_location::current(), u"no sink");
        Require(logger.Flush(std::chrono::seconds(5)), "Flush should drain even when no sinks exist");
        Require(first->Count() == 1, "First sink should remain unchanged after ClearSinks");
        Require(second->Count() == 1, "Second sink should remain unchanged after ClearSinks");

        logger.Shutdown(true);
    }

    void TestLoggerDiagnosticsExport() {
        auto& logger = LikesProgram::Log::Logger::Instance(false, true);
        ResetLogger(logger);

        auto sink = std::make_shared<CountingSink>();
        LikesProgram::Log::LoggerOptions options;
        options.maxQueueSize = 8;
        options.overflowPolicy = LikesProgram::Log::QueueOverflowPolicy::DropOldest;
        options.enqueueTimeout = std::chrono::milliseconds(12);
        options.outputFormat = LikesProgram::Log::LogOutputFormat::JsonLines;
        logger.Configure(options);
        logger.SetLevel(LikesProgram::Log::Level::Warn);
        logger.SetLoggerName(u"diagnostics \"logger\"");
        logger.AddSink(sink);

        Require(logger.Start(), "Logger should start for diagnostics export test");
        logger.Log(LikesProgram::Log::Level::Error, std::source_location::current(), u"diagnostic message");
        Require(logger.Flush(std::chrono::seconds(5)), "Flush should drain diagnostics message");

        auto diagnostics = logger.Diagnostics();
        Require(diagnostics.stats.running, "Diagnostics should report running logger");
        Require(diagnostics.stats.sinkCount == 1, "Diagnostics should include sink count");
        Require(diagnostics.stats.currentQueueSize == 0, "Diagnostics should report drained queue");
        Require(diagnostics.stats.retryQueueSize == 0, "Diagnostics should report drained retry queue");
        Require(diagnostics.stats.acceptedMessages >= 1, "Diagnostics should include accepted count");
        Require(diagnostics.stats.processedMessages >= 1, "Diagnostics should include processed count");
        Require(diagnostics.minLevel == LikesProgram::Log::Level::Warn,
            "Diagnostics should include current level");
        Require(diagnostics.options.maxQueueSize == 8, "Diagnostics should include queue size option");
        Require(diagnostics.options.overflowPolicy == LikesProgram::Log::QueueOverflowPolicy::DropOldest,
            "Diagnostics should include overflow policy option");
        Require(diagnostics.options.outputFormat == LikesProgram::Log::LogOutputFormat::JsonLines,
            "Diagnostics should include output format option");
        Require(diagnostics.options.enqueueTimeout == std::chrono::milliseconds(12),
            "Diagnostics should include enqueue timeout option");
        Require(diagnostics.loggerName == u"diagnostics \"logger\"",
            "Diagnostics should include logger name");
        Require(diagnostics.debug, "Diagnostics should include debug flag");

        auto text = logger.ExportDiagnostics();
        Require(text.StartsWith(u"LoggerDiagnostics{"), "Text diagnostics should use stable prefix");
        Require(text.Find(u"level=Warn") != LikesProgram::String::npos,
            "Text diagnostics should include level");
        Require(text.Find(u"output_format=JsonLines") != LikesProgram::String::npos,
            "Text diagnostics should include output format");
        Require(text.Find(u"overflow_policy=DropOldest") != LikesProgram::String::npos,
            "Text diagnostics should include overflow policy");
        Require(text.Find(u"retry_queue_high_watermark") != LikesProgram::String::npos,
            "Text diagnostics should include retry queue high watermark");
        Require(text.Find(u"sink_count=1") != LikesProgram::String::npos,
            "Text diagnostics should include sink count");

        auto json = logger.ExportDiagnostics(LikesProgram::Log::LogOutputFormat::JsonLines);
        Require(json.StartsWith(u"{"), "Json diagnostics should start with object");
        Require(json.EndsWith(u"}"), "Json diagnostics should end with object");
        Require(json.Find(u"\"kind\":\"logger_diagnostics\"") != LikesProgram::String::npos,
            "Json diagnostics should include kind");
        Require(json.Find(u"\"level\":\"Warn\"") != LikesProgram::String::npos,
            "Json diagnostics should include level");
        Require(json.Find(u"\"output_format\":\"JsonLines\"") != LikesProgram::String::npos,
            "Json diagnostics should include output format");
        Require(json.Find(u"\"overflow_policy\":\"DropOldest\"") != LikesProgram::String::npos,
            "Json diagnostics should include overflow policy");
        Require(json.Find(u"\"logger\":\"diagnostics \\\"logger\\\"\"") != LikesProgram::String::npos,
            "Json diagnostics should escape logger name");
        Require(json.Find(u"\"sink_count\":1") != LikesProgram::String::npos,
            "Json diagnostics should include numeric sink count");
        Require(json.Find(u"\"retry_queue_size\":0") != LikesProgram::String::npos,
            "Json diagnostics should include retry queue size");
        Require(json.Find(u"\"running\":true") != LikesProgram::String::npos,
            "Json diagnostics should include running flag");

        logger.Shutdown(true);
    }

    void TestTextContextFields() {
        auto& logger = LikesProgram::Log::Logger::Instance(false, false);
        ResetLogger(logger);

        auto sink = std::make_shared<CapturingSink>();
        logger.SetLoggerName(u"main-logger");
        LikesProgram::Log::Logger::SetThreadName(u"worker-a");
        LikesProgram::Log::Logger::SetModule(u"orders");
        LikesProgram::Log::Logger::SetCategory(u"checkout");
        LikesProgram::Log::Logger::SetTraceId(u"trace-1");
        LikesProgram::Log::Logger::SetContextField(u"tenant", u"blue");
        logger.AddSink(sink);

        Require(logger.Start(), "Logger should start for text context test");
        logger.Log(LikesProgram::Log::Level::Info, std::source_location::current(), u"context text");
        Require(logger.Flush(std::chrono::seconds(5)), "Flush should drain text context test");

        auto lines = sink->Lines();
        Require(lines.size() == 1, "CapturingSink should receive one text line");
        Require(lines[0].Find(u"[T:worker-a]") != LikesProgram::String::npos,
            "Text format should use thread name");
        Require(lines[0].Find(u"[Logger:main-logger]") != LikesProgram::String::npos,
            "Text format should include logger name");
        Require(lines[0].Find(u"[Module:orders]") != LikesProgram::String::npos,
            "Text format should include module");
        Require(lines[0].Find(u"[Category:checkout]") != LikesProgram::String::npos,
            "Text format should include category");
        Require(lines[0].Find(u"[TraceId:trace-1]") != LikesProgram::String::npos,
            "Text format should include trace id");
        Require(lines[0].Find(u"[Context.tenant:blue]") != LikesProgram::String::npos,
            "Text format should include custom context field");

        logger.Shutdown(true);
        LikesProgram::Log::Logger::ClearThreadName();
        LikesProgram::Log::Logger::ClearContext();
    }

    void TestJsonLinesContextFields() {
        auto& logger = LikesProgram::Log::Logger::Instance(false, false);
        ResetLogger(logger);

        auto sink = std::make_shared<CapturingSink>();
        LikesProgram::Log::LoggerOptions options;
        options.outputFormat = LikesProgram::Log::LogOutputFormat::JsonLines;
        logger.Configure(options);
        logger.SetLoggerName(u"json-logger");
        LikesProgram::Log::Logger::SetThreadName(u"json-thread");
        LikesProgram::Log::Logger::SetRequestId(u"req-42");
        LikesProgram::Log::Logger::SetContextField(u"path", u"/api/checkout");
        LikesProgram::Log::Logger::SetContextField(u"quote", u"say \"hi\"");
        logger.AddSink(sink);

        Require(logger.Start(), "Logger should start for json context test");
        logger.Log(LikesProgram::Log::Level::Warn, std::source_location::current(),
            u"json message {}", 7);
        Require(logger.Flush(std::chrono::seconds(5)), "Flush should drain json context test");

        auto lines = sink->Lines();
        Require(lines.size() == 1, "CapturingSink should receive one json line");
        Require(lines[0].StartsWith(u"{"), "Json lines format should start with object");
        Require(lines[0].EndsWith(u"}"), "Json lines format should end with object");
        Require(lines[0].Find(u"\"level\":\"Warn\"") != LikesProgram::String::npos,
            "Json format should include level");
        Require(lines[0].Find(u"\"logger\":\"json-logger\"") != LikesProgram::String::npos,
            "Json format should include logger name");
        Require(lines[0].Find(u"\"thread\":\"json-thread\"") != LikesProgram::String::npos,
            "Json format should include thread name");
        Require(lines[0].Find(u"\"request_id\":\"req-42\"") != LikesProgram::String::npos,
            "Json format should include request id");
        Require(lines[0].Find(u"\"message\":\"json message 7\"") != LikesProgram::String::npos,
            "Json format should include formatted message");
        Require(lines[0].Find(u"\"quote\":\"say \\\"hi\\\"\"") != LikesProgram::String::npos,
            "Json format should escape context value");

        logger.Shutdown(true);
        LikesProgram::Log::Logger::ClearThreadName();
        LikesProgram::Log::Logger::ClearContext();
    }

    void TestContextScopeRestore() {
        auto& logger = LikesProgram::Log::Logger::Instance(false, false);
        ResetLogger(logger);

        auto sink = std::make_shared<CapturingSink>();
        LikesProgram::Log::Logger::SetModule(u"outer");
        LikesProgram::Log::Logger::SetTraceId(u"trace-outer");
        LikesProgram::Log::Logger::SetContextField(u"tenant", u"blue");
        logger.AddSink(sink);

        Require(logger.Start(), "Logger should start for context scope test");
        {
            LikesProgram::Log::LoggerContextScope scope(u"tenant", u"green");
            scope.SetModule(u"inner");
            scope.SetTraceId(u"trace-inner");
            scope.SetContextField(u"operation", u"create");
            logger.Log(LikesProgram::Log::Level::Info, std::source_location::current(), u"inside scope");
        }
        logger.Log(LikesProgram::Log::Level::Info, std::source_location::current(), u"outside scope");
        Require(logger.Flush(std::chrono::seconds(5)), "Flush should drain context scope test");

        auto lines = sink->Lines();
        Require(lines.size() == 2, "CapturingSink should receive scoped and restored lines");
        Require(lines[0].Find(u"[Module:inner]") != LikesProgram::String::npos,
            "Scope should override module inside scope");
        Require(lines[0].Find(u"[TraceId:trace-inner]") != LikesProgram::String::npos,
            "Scope should override trace id inside scope");
        Require(lines[0].Find(u"[Context.tenant:green]") != LikesProgram::String::npos,
            "Scope should override custom field inside scope");
        Require(lines[0].Find(u"[Context.operation:create]") != LikesProgram::String::npos,
            "Scope should add custom field inside scope");
        Require(lines[1].Find(u"[Module:outer]") != LikesProgram::String::npos,
            "Scope should restore outer module after destruction");
        Require(lines[1].Find(u"[TraceId:trace-outer]") != LikesProgram::String::npos,
            "Scope should restore outer trace id after destruction");
        Require(lines[1].Find(u"[Context.tenant:blue]") != LikesProgram::String::npos,
            "Scope should restore outer custom field after destruction");
        Require(lines[1].Find(u"operation") == LikesProgram::String::npos,
            "Scope-only custom field should be removed after destruction");

        logger.Shutdown(true);
        LikesProgram::Log::Logger::ClearContext();
    }

    void TestJsonContextFieldDoesNotOverrideBuiltins() {
        auto& logger = LikesProgram::Log::Logger::Instance(false, false);
        ResetLogger(logger);

        auto sink = std::make_shared<CapturingSink>();
        LikesProgram::Log::LoggerOptions options;
        options.outputFormat = LikesProgram::Log::LogOutputFormat::JsonLines;
        logger.Configure(options);
        LikesProgram::Log::Logger::SetModule(u"builtin-module");
        LikesProgram::Log::Logger::SetContextField(u"module", u"context-module");
        logger.AddSink(sink);

        Require(logger.Start(), "Logger should start for json conflict test");
        logger.Log(LikesProgram::Log::Level::Info, std::source_location::current(), u"conflict");
        Require(logger.Flush(std::chrono::seconds(5)), "Flush should drain json conflict test");

        auto lines = sink->Lines();
        Require(lines.size() == 1, "CapturingSink should receive one json conflict line");
        Require(lines[0].Find(u"\"module\":\"builtin-module\"") != LikesProgram::String::npos,
            "Builtin module should stay as top-level json field");
        Require(lines[0].Find(u"\"context\":{\"module\":\"context-module\"}") != LikesProgram::String::npos,
            "Custom module field should stay inside context object");

        logger.Shutdown(true);
        LikesProgram::Log::Logger::ClearContext();
    }
}

int main() {
    try {
        TestPackageIdentity();
        TestLevelConversion();
        TestDisabledLevelSkipsFormatting();
        TestLoggerFileSink();
        TestFileSinkOpenFailureBoundary();
        TestConsoleSinkWriteBoundary();
        TestFileSinkRetentionPolicy();
        TestLoggerFlushAndStats();
        TestSinkFailureIsolation();
        TestQueueBackpressureDropNewest();
        TestQueueBackpressureDropOldest();
        TestShutdownTimeoutRecovery();
        TestSinkFlushFailureIsolation();
        TestLoggerConfigOpenSinkAndRetry();
        TestLoggerRetryQueueBounded();
        TestLoggerFlushUsesSingleTimeoutBudgetForRetryDrain();
        TestLoggerShutdownStopsRetryExpansion();
        TestLoggerConfigValidation();
        TestLoggerConfigureNormalizesInvalidRuntimeOptions();
        TestFileSinkMultiProcessConfig();
        TestFileSinkMultiProcessRotationSeesPeerWrites();
        TestLoggerStress100k();
        TestRuntimeSinkReplacement();
        TestLoggerDiagnosticsExport();
        TestTextContextFields();
        TestJsonLinesContextFields();
        TestContextScopeRestore();
        TestJsonContextFieldDoesNotOverrideBuiltins();
    }
    catch (const std::exception& ex) {
        std::cerr << ex.what() << std::endl;
        return 1;
    }

    return 0;
}
