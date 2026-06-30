# LikesProgramLogging 使用手册

`LikesProgramLogging` 是 LikesProgram 的轻量异步日志扩展包。它只依赖 `LikesProgramCore`，不依赖 Metrics、Threading、Net 或第三方日志库。

它适合二次开发时快速接入：

- 按级别过滤日志。
- 后台线程异步分发日志。
- 输出到控制台或文件。
- 支持文本格式和 JSON Lines。
- 支持 logger name、thread name、module、category、trace id、span id、request id 和自定义上下文字段。
- 支持按 Sink 失败重试、退避和有界重试队列。
- 支持文件轮转、保留策略和可选多进程同文件写入。
- 支持运行时替换 Sink、开放式 `LoggerConfig` 配置和导出诊断信息。

## 构建和链接

Logging 默认不构建。使用前需要在配置 LikesProgram 时显式开启：

```powershell
cd C:\Users\TX2\Desktop\LikesProgramProjects\LikesProgram
cmake -S . -B build-logging -DLIKESPROGRAM_BUILD_LOGGING=ON
cmake --build build-logging --config Debug
ctest --test-dir build-logging -R LikesProgramLoggingTests --output-on-failure -C Debug
```

在外部项目中链接：

```cmake
find_package(LikesProgram CONFIG REQUIRED)

add_executable(MyApp main.cpp)
target_link_libraries(MyApp PRIVATE LikesProgram::Logging)
```

`LikesProgram::Logging` 会传递依赖 `LikesProgram::Core`。你也可以为了阅读清楚显式写上：

```cmake
target_link_libraries(MyApp PRIVATE
    LikesProgram::Core
    LikesProgram::Logging
)
```

## 最小可运行示例

```cpp
#include <LikesProgram/Logging/Logger.hpp>
#include <LikesProgram/Logging/sinks/ConsoleSink.hpp>

#include <chrono>
#include <source_location>

int main() {
    auto& logger = LikesProgram::Log::Logger::Instance(true, true);

    logger.SetLevel(LikesProgram::Log::Level::Info);
    logger.SetLoggerName(u"demo");
    logger.AddSink(LikesProgram::Log::ConsoleSink::CreateSink());

    logger.Log(LikesProgram::Log::Level::Info, std::source_location::current(),
        u"service {} started on port {}", u"orders", 8080);

    logger.Flush(std::chrono::seconds(5));
    logger.Shutdown();
    return 0;
}
```

解释：

- `Instance(true, true)` 获取全局 Logger，并自动启动后台线程，第二个 `true` 表示输出 source location 调试信息。
- `SetLevel(Info)` 表示低于 Info 的 Trace/Debug 会被过滤。
- `AddSink(ConsoleSink::CreateSink())` 添加控制台输出。
- `Log(...)` 是异步入队，真正写入由后台线程完成。
- `Flush(...)` 等待队列处理完并刷新所有 Sink。
- `Shutdown()` 停止后台线程，默认清空 Sink 列表。

## 公开头文件

优先使用聚合头：

```cpp
#include <LikesProgram/Logging/Logging.hpp>
```

也可以按需包含叶子头：

```cpp
#include <LikesProgram/Logging/LoggerType.hpp>
#include <LikesProgram/Logging/Logger.hpp>
#include <LikesProgram/Logging/LoggerConfig.hpp>
#include <LikesProgram/Logging/sinks/Sink.hpp>
#include <LikesProgram/Logging/sinks/ConsoleSink.hpp>
#include <LikesProgram/Logging/sinks/FileSink.hpp>
```

## 包信息 API

这些 API 可用于启动日志、诊断输出、插件加载检查等场景。

```cpp
#include <LikesProgram/Logging/Logging.hpp>

const char* name = LikesProgram::Logging::PackageName();       // "LikesProgramLogging"
const char* version = LikesProgram::Logging::PackageVersion(); // 跟随 LikesProgram 版本
bool ok = LikesProgram::Logging::PackageAvailable();           // true 表示已链接到当前进程
```

返回值是 C 字符串或 bool，不需要调用方释放内存。

## 日志级别

级别从低到高：

```cpp
LikesProgram::Log::Level::Trace;
LikesProgram::Log::Level::Debug;
LikesProgram::Log::Level::Info;
LikesProgram::Log::Level::Warn;
LikesProgram::Log::Level::Error;
LikesProgram::Log::Level::Fatal;
```

设置过滤级别：

```cpp
logger.SetLevel(LikesProgram::Log::Level::Warn);
```

此时 Trace、Debug、Info 都不会入队。被过滤的日志不会格式化参数，因此禁用级别下的昂贵参数也不会被拷贝或格式化。

级别和文本互转：

```cpp
using namespace LikesProgram::Log;

LikesProgram::String text = LevelToString(Level::Warn); // "Warn"
Level level = StringToLevel(u"error");                  // Level::Error
Level fallback = StringToLevel(u"missing", Level::Info);
```

`StringToLevel` 不区分大小写，无法识别时返回 `defaultLevel`，默认是 `Trace`。

## 获取和启动 Logger

`Logger` 是进程内全局单例：

```cpp
auto& logger1 = LikesProgram::Log::Logger::Instance();
auto& logger2 = LikesProgram::Log::Logger::Instance(true);
auto& logger3 = LikesProgram::Log::Logger::Instance(true, true);
```

兼容别名：

```cpp
auto& logger = LikesProgram::Logger::Instance(true); // 等价 LikesProgram::Log::Logger::Instance(true)
```

区别：

- `Instance()`：只获取单例，不自动启动。
- `Instance(true)`：获取单例，如果还没启动或已关闭，则启动。
- `Instance(true, debug)`：同时设置是否输出调用点调试信息。

手动启动：

```cpp
auto& logger = LikesProgram::Log::Logger::Instance();
logger.AddSink(LikesProgram::Log::ConsoleSink::CreateSink());
bool started = logger.Start();
```

`Start()` 可重复调用。已启动时返回 `true`。

关闭：

```cpp
logger.Shutdown();                         // 等待 drain，清空 Sink
logger.Shutdown(false);                    // 等待 drain，不清空 Sink
bool ok = logger.Shutdown(std::chrono::seconds(5), true);
```

带超时的 `Shutdown(timeout, clearSink)`：

- 会请求停止后台线程。
- 会调用 `Flush(timeout)` 等待队列清空。
- 超时返回 `false`。
- 如果短超时失败，可以释放阻塞资源后再次调用更长超时的 `Shutdown`。
- `clearSink=true` 时成功关闭后清空 Sink 列表。

## 写日志

推荐直接调用 `Logger::Log`：

```cpp
logger.Log(LikesProgram::Log::Level::Info, std::source_location::current(),
    u"user {} paid {}", userId, amount);
```

格式串使用 `LikesProgram::String::Format` 语法。更多语法见 Core 使用手册中的 `String::Format` 章节。

无参数日志不会走格式化：

```cpp
logger.Log(LikesProgram::Log::Level::Info, std::source_location::current(),
    u"service started");
```

便捷宏：

```cpp
LogTrace(u"trace {}", id);
LogDebug(u"debug {}", id);
LogInfo(u"hello {}", name);
LogWarn(u"slow request {}", ms);
LogError(u"failed: {}", reason);
LogFatal(u"fatal: {}", reason);
```

宏会使用全局 `Logger::Instance()` 和 `std::source_location::current()`。使用宏前先配置 Sink 并启动 Logger，这样日志才会进入正常输出流程。

如果 Logger 未启动，写日志会增加 `droppedMessages`，不会自动启动。

## 输出编码

设置 Sink 输出编码：

```cpp
logger.SetEncoding(LikesProgram::String::Encoding::UTF8);
logger.SetEncoding(LikesProgram::String::Encoding::GBK);
```

默认是 UTF-8。编码快照会写入每条 `Message`，由 Sink 在输出时使用。文件 Sink 会按该编码写入文件。

## LoggerOptions 队列配置

`LoggerOptions` 控制异步队列和输出格式：

```cpp
LikesProgram::Log::LoggerOptions options;
options.maxQueueSize = 65536;
options.overflowPolicy = LikesProgram::Log::QueueOverflowPolicy::Block;
options.enqueueTimeout = std::chrono::milliseconds(100);
options.outputFormat = LikesProgram::Log::LogOutputFormat::Text;

logger.Configure(options);
```

字段说明：

| 字段 | 默认值 | 说明 |
| --- | --- | --- |
| `maxQueueSize` | `65536` | 异步队列上限，`0` 表示不限制 |
| `overflowPolicy` | `Block` | 队列满时的策略 |
| `enqueueTimeout` | `100ms` | `Block` 策略等待容量的最长时间 |
| `outputFormat` | `Text` | 日志输出格式，`Text` 或 `JsonLines` |

背压策略：

```cpp
options.overflowPolicy = LikesProgram::Log::QueueOverflowPolicy::Block;
```

`Block`：

- 队列满时写日志的线程等待容量。
- `enqueueTimeout > 0` 时等待超时会丢弃本条日志，`enqueueTimeouts` 和 `droppedMessages` 增加。
- `enqueueTimeout <= 0` 时一直等到有容量或 Logger 停止。

```cpp
options.overflowPolicy = LikesProgram::Log::QueueOverflowPolicy::DropNewest;
```

`DropNewest`：

- 队列满时丢弃当前新日志。
- `droppedMessages` 增加。

```cpp
options.overflowPolicy = LikesProgram::Log::QueueOverflowPolicy::DropOldest;
```

`DropOldest`：

- 队列满时先丢弃队列中最旧的一条，再放入当前新日志。
- `droppedMessages` 增加。

## Sink 管理

添加 Sink：

```cpp
logger.AddSink(LikesProgram::Log::ConsoleSink::CreateSink());
```

一次性替换 Sink：

```cpp
std::vector<std::shared_ptr<LikesProgram::Log::Sink>> sinks;
sinks.push_back(LikesProgram::Log::ConsoleSink::CreateSink());

logger.SetSinks(sinks);
```

清空 Sink：

```cpp
logger.ClearSinks();
```

行为说明：

- `AddSink(nullptr)` 会被忽略。
- `SetSinks` 会过滤空指针，然后原子替换当前列表。
- `ClearSinks` 后日志仍可入队和统计，但不会输出到任何地方。
- 后台分发线程调用 Sink 时会持有共享锁；替换 Sink 会等待当前分发批次结束后生效。
- 某个 Sink 的 `Write` 或 `Flush` 抛异常不会影响其他 Sink，失败会计入 `sinkWriteFailures` 并写到 `std::cerr`。

## LoggerConfig 开放式配置

`LoggerConfig` 是 Logging 的原生运行时配置模型。它不依赖 `LikesProgramConfig`，也不使用封闭的 `SinkKind` 枚举；每个 `SinkConfig` 都直接包装一个 `std::shared_ptr<Sink>`，因此可以接入内置 Sink 或业务自定义 Sink。

```cpp
#include <LikesProgram/Logging/Logging.hpp>

#include <chrono>
#include <stdexcept>

LikesProgram::Log::LoggerConfig config;
config.level = LikesProgram::Log::Level::Info;
config.loggerName = u"orders-service";
config.encoding = LikesProgram::String::Encoding::UTF8;
config.debug = true;
config.options.maxQueueSize = 65536;
config.options.overflowPolicy = LikesProgram::Log::QueueOverflowPolicy::Block;
config.options.enqueueTimeout = std::chrono::milliseconds(100);
config.options.outputFormat = LikesProgram::Log::LogOutputFormat::JsonLines;

LikesProgram::Log::ConsoleSinkConfig console;
console.minLevel = LikesProgram::Log::Level::Warn;
auto consoleSink = LikesProgram::Log::MakeConsoleSinkConfig(console);

LikesProgram::Log::FileSinkConfig file;
file.name = u"service-file";
file.path = u"./logs";
file.filename = u"orders.log";
file.minLevel = LikesProgram::Log::Level::Info;
file.fileOptions.maxFileSizeMB = 64;
file.fileOptions.retentionDays = 14;
file.retry.enabled = true;
file.retry.maxAttempts = 3;
file.retry.maxQueueSize = 1024;
file.retry.initialBackoff = std::chrono::milliseconds(50);
auto fileSink = LikesProgram::Log::MakeFileSinkConfig(file);

if (!consoleSink.IsOk()) {
    throw std::runtime_error(consoleSink.GetStatus().ToString().ToStdString());
}
if (!fileSink.IsOk()) {
    throw std::runtime_error(fileSink.GetStatus().ToString().ToStdString());
}
config.sinks.push_back(consoleSink.Value());
config.sinks.push_back(fileSink.Value());

auto& logger = LikesProgram::Log::Logger::Instance(false, false);
auto status = logger.ApplyConfig(config);
if (!status.IsOk()) {
    throw std::runtime_error(status.ToString().ToStdString());
}
if (!logger.Start()) {
    throw std::runtime_error("failed to start logger");
}
```

`ValidateLoggerConfig(config)` 可以在应用前单独校验。`Logger::ApplyConfig` 会先校验，失败时保留当前运行配置。

`EffectiveConfig()` 可以导出当前生效配置快照：

```cpp
auto effective = logger.EffectiveConfig();
if (effective.IsOk()) {
    auto currentLevel = effective.Value().level;
}
```

## 使用 LikesProgramConfig 配置 Logging

`LikesProgramLogging` 代码本身不依赖 `LikesProgramConfig`。如果你的应用同时启用了 Config 包，可以在应用层读取配置值树，再手动映射到 `LoggerConfig`。

示例 TOML：

```toml
[logging]
level = "info"
logger = "orders-service"
debug = true
format = "json"
encoding = "utf8"

[logging.queue]
max_size = 65536
overflow = "block"
enqueue_timeout_ms = 100

[logging.console]
enabled = true
min_level = "warn"

[logging.file]
enabled = true
path = "./logs"
filename = "orders.log"
min_level = "info"
max_file_size_mb = 64
retention_days = 14
max_retained_files = 128
multi_process = false

[logging.file.retry]
enabled = true
max_attempts = 3
max_queue_size = 1024
initial_backoff_ms = 50
max_backoff_ms = 500
```

映射代码：

```cpp
#include <LikesProgram/Config/Config.hpp>
#include <LikesProgram/Logging/Logging.hpp>

#include <chrono>
#include <stdexcept>

namespace {
    LikesProgram::Log::Level ReadLevel(
        const LikesProgram::Config::Configuration& config,
        const LikesProgram::String& key,
        LikesProgram::Log::Level fallback) {
        return LikesProgram::Log::StringToLevel(config.GetString(key, LikesProgram::Log::LevelToString(fallback)),
            fallback);
    }

    LikesProgram::Log::LogOutputFormat ReadFormat(
        const LikesProgram::Config::Configuration& config,
        const LikesProgram::String& key) {
        auto text = config.GetString(key, u"text").ToLower();
        return text == u"json" || text == u"json_lines" || text == u"jsonlines"
            ? LikesProgram::Log::LogOutputFormat::JsonLines
            : LikesProgram::Log::LogOutputFormat::Text;
    }

    LikesProgram::String::Encoding ReadEncoding(
        const LikesProgram::Config::Configuration& config,
        const LikesProgram::String& key) {
        auto text = config.GetString(key, u"utf8").ToLower();
        if (text == u"gbk") return LikesProgram::String::Encoding::GBK;
        if (text == u"utf16" || text == u"utf-16") return LikesProgram::String::Encoding::UTF16;
        if (text == u"utf32" || text == u"utf-32") return LikesProgram::String::Encoding::UTF32;
        return LikesProgram::String::Encoding::UTF8;
    }

    LikesProgram::Log::QueueOverflowPolicy ReadOverflow(
        const LikesProgram::Config::Configuration& config,
        const LikesProgram::String& key) {
        auto text = config.GetString(key, u"block").ToLower();
        if (text == u"drop_newest" || text == u"drop-newest" || text == u"dropnewest") {
            return LikesProgram::Log::QueueOverflowPolicy::DropNewest;
        }
        if (text == u"drop_oldest" || text == u"drop-oldest" || text == u"dropoldest") {
            return LikesProgram::Log::QueueOverflowPolicy::DropOldest;
        }
        return LikesProgram::Log::QueueOverflowPolicy::Block;
    }

    LikesProgram::Log::RetryConfig ReadRetry(
        const LikesProgram::Config::Configuration& config,
        const LikesProgram::String& prefix) {
        LikesProgram::Log::RetryConfig retry;
        retry.enabled = config.GetBool(prefix + u".enabled", false);
        retry.maxAttempts = static_cast<uint32_t>(config.GetInt64(prefix + u".max_attempts", 0));
        retry.maxQueueSize = static_cast<size_t>(config.GetInt64(prefix + u".max_queue_size", 4096));
        retry.initialBackoff = std::chrono::milliseconds(
            config.GetInt64(prefix + u".initial_backoff_ms", 0));
        retry.maxBackoff = std::chrono::milliseconds(
            config.GetInt64(prefix + u".max_backoff_ms", 0));
        return retry;
    }

    LikesProgram::Log::LoggerConfig BuildLoggerConfig(
        const LikesProgram::Config::Configuration& config) {
        LikesProgram::Log::LoggerConfig loggerConfig;
        loggerConfig.level = ReadLevel(config, u"logging.level", LikesProgram::Log::Level::Info);
        loggerConfig.loggerName = config.GetString(u"logging.logger", u"service");
        loggerConfig.debug = config.GetBool(u"logging.debug", false);
        loggerConfig.encoding = ReadEncoding(config, u"logging.encoding");

        loggerConfig.options.maxQueueSize = static_cast<size_t>(
            config.GetInt64(u"logging.queue.max_size", 65536));
        loggerConfig.options.overflowPolicy = ReadOverflow(config, u"logging.queue.overflow");
        loggerConfig.options.enqueueTimeout = std::chrono::milliseconds(
            config.GetInt64(u"logging.queue.enqueue_timeout_ms", 100));
        loggerConfig.options.outputFormat = ReadFormat(config, u"logging.format");

        if (config.GetBool(u"logging.console.enabled", true)) {
            LikesProgram::Log::ConsoleSinkConfig console;
            console.minLevel = ReadLevel(config, u"logging.console.min_level", loggerConfig.level);
            console.overrideOutputFormat = true;
            console.outputFormat = loggerConfig.options.outputFormat;
            auto sink = LikesProgram::Log::MakeConsoleSinkConfig(console);
            if (!sink.IsOk()) {
                throw std::runtime_error(sink.GetStatus().ToString().ToStdString());
            }
            loggerConfig.sinks.push_back(sink.Value());
        }

        if (config.GetBool(u"logging.file.enabled", false)) {
            LikesProgram::Log::FileSinkConfig file;
            file.path = config.GetString(u"logging.file.path", u"./logs");
            file.filename = config.GetString(u"logging.file.filename", u"service.log");
            file.minLevel = ReadLevel(config, u"logging.file.min_level", loggerConfig.level);
            file.overrideOutputFormat = true;
            file.outputFormat = loggerConfig.options.outputFormat;
            file.fileOptions.maxFileSizeMB = static_cast<size_t>(
                config.GetInt64(u"logging.file.max_file_size_mb", 30));
            file.fileOptions.retentionDays = static_cast<uint32_t>(
                config.GetInt64(u"logging.file.retention_days", 0));
            file.fileOptions.maxRetainedFiles = static_cast<size_t>(
                config.GetInt64(u"logging.file.max_retained_files", 0));
            file.multiProcess.enabled = config.GetBool(u"logging.file.multi_process", false);
            file.retry = ReadRetry(config, u"logging.file.retry");

            auto sink = LikesProgram::Log::MakeFileSinkConfig(file);
            if (!sink.IsOk()) {
                throw std::runtime_error(sink.GetStatus().ToString().ToStdString());
            }
            loggerConfig.sinks.push_back(sink.Value());
        }

        return loggerConfig;
    }
}

void ConfigureLoggingFromToml(const LikesProgram::String& tomlText) {
    auto config = LikesProgram::Config::Configuration::FromToml(tomlText);
    auto loggerConfig = BuildLoggerConfig(config);

    auto& logger = LikesProgram::Log::Logger::Instance(false, false);
    auto status = logger.ApplyConfig(loggerConfig);
    if (!status.IsOk()) {
        throw std::runtime_error(status.ToString().ToStdString());
    }

    if (!logger.Start()) {
        throw std::runtime_error("failed to start logger");
    }
}
```

这个例子把格式解析留在应用层，因此你可以按业务需要换成 JSON、YAML、环境变量或命令行参数，而 Logging 包仍保持只依赖 Core。

## ConsoleSink

控制台 Sink 输出到当前进程标准输出。

```cpp
#include <LikesProgram/Logging/sinks/ConsoleSink.hpp>

auto sink = LikesProgram::Log::ConsoleSink::CreateSink();
logger.AddSink(sink);
```

也可以直接构造：

```cpp
LikesProgram::Log::ConsoleSink sink;
```

`ConsoleSink::Write` 会按日志级别应用控制台颜色，然后写出统一格式化后的日志文本。

## FileSink

文件 Sink 按日期目录写文件，并支持大小轮转和保留策略。

最简单用法：

```cpp
#include <LikesProgram/Logging/sinks/FileSink.hpp>

auto sink = LikesProgram::Log::FileSink::CreateSink(u"./logs", u"service.log");
logger.AddSink(sink);
```

参数为空时：

- `path` 为空使用 `./logs`
- `filename` 为空使用 `Logger.log`

输出目录形态：

```text
logs/
  2026-06-29/
    service.log
    1_service.log
    2_service.log
```

完整选项：

```cpp
LikesProgram::Log::FileSinkOptions options;
options.maxFileSizeMB = 64;
options.maxFileSizeBytes = 0;
options.retentionDays = 14;
options.maxRetainedFiles = 128;
options.maxTotalSizeBytes = 1024ull * 1024ull * 1024ull;

auto sink = LikesProgram::Log::FileSink::CreateSink(u"./logs", u"service.log", options);
logger.AddSink(sink);
```

字段说明：

| 字段 | 默认值 | 说明 |
| --- | --- | --- |
| `maxFileSizeMB` | `30` | 单文件最大 MB，`0` 表示不按 MB 配置大小轮转 |
| `maxFileSizeBytes` | `0` | 精确字节上限，非 0 时优先于 `maxFileSizeMB` |
| `retentionDays` | `0` | 按文件修改时间保留天数，`0` 表示不按天清理 |
| `maxRetainedFiles` | `0` | 最多保留托管日志文件数，`0` 表示不限制 |
| `maxTotalSizeBytes` | `0` | 托管日志文件总字节上限，`0` 表示不限制 |

运行中更新策略：

```cpp
auto fileSink = std::make_shared<LikesProgram::Log::FileSink>(
    u"./logs", u"service.log", options);

logger.AddSink(fileSink);

options.maxRetainedFiles = 32;
fileSink->Configure(options);

auto current = fileSink->Options();
```

轮转规则：

- 日期变化会重新打开当天目录下的日志文件。
- 当前文件大小达到上限后，下一次写入前打开 `1_filename`、`2_filename` 等轮转文件。
- 保留策略只管理当前 `filename` 命名规则匹配的文件，不删除其他文件。
- 清理时不会删除当前正在打开的文件。

错误边界：

- 日志根路径无法创建目录、被普通文件占用或文件无法打开时，构造 `FileSink` 会抛异常。
- 写入和 flush 失败时，异常会被 Logger 捕获并计入 `sinkWriteFailures`。
- `FileSink` 析构时会尝试 flush，但析构不会向外抛异常。

## 自定义 Sink

继承 `Sink` 并实现 `Write`：

```cpp
#include <LikesProgram/Logging/sinks/Sink.hpp>

#include <mutex>
#include <vector>

class MemorySink final : public LikesProgram::Log::Sink {
public:
    MemorySink() : Sink(u"MemorySink") {}

    void Write(const LikesProgram::Log::Message& message) override {
        auto line = FormatLogMessage(message);
        std::lock_guard<std::mutex> lock(mutex_);
        lines_.push_back(line);
    }

    void Flush() override {
        // 如果有缓冲，在这里提交。没有缓冲可以不重写。
    }

private:
    std::mutex mutex_;
    std::vector<LikesProgram::String> lines_;
};
```

要点：

- `Sink` 构造函数的名称会出现在文本日志的 `[SinkName]` 字段和 JSON Lines 的 `"sink"` 字段。
- `FormatLogMessage(message)` 可复用内置文本/JSON 格式。
- `Write` 可能被后台线程调用；如果 Sink 自己保存共享数据，需要在 Sink 里加锁。
- `Flush` 默认无操作。

## 上下文字段

设置全局 Logger 名称：

```cpp
logger.SetLoggerName(u"orders-service");
```

设置当前线程上下文：

```cpp
LikesProgram::Log::Logger::SetThreadName(u"worker-1");
LikesProgram::Log::Logger::SetModule(u"orders");
LikesProgram::Log::Logger::SetCategory(u"checkout");
LikesProgram::Log::Logger::SetTraceId(u"trace-123");
LikesProgram::Log::Logger::SetSpanId(u"span-456");
LikesProgram::Log::Logger::SetRequestId(u"req-789");
LikesProgram::Log::Logger::SetContextField(u"tenant", u"blue");
```

删除或清空：

```cpp
LikesProgram::Log::Logger::RemoveContextField(u"tenant");
LikesProgram::Log::Logger::ClearThreadName();
LikesProgram::Log::Logger::ClearContext();
```

`ClearContext()` 会清空 module、category、traceId、spanId、requestId 和自定义字段，但不会清空 thread name。清 thread name 要调用 `ClearThreadName()`。

上下文是线程局部的：

- 一个线程设置的 module/traceId 不会影响另一个线程。
- 写入日志时会复制上下文快照到 `Message`。
- 后续修改上下文不会改变已经入队的日志。

临时上下文：

```cpp
LikesProgram::Log::Logger::SetModule(u"outer");
LikesProgram::Log::Logger::SetContextField(u"tenant", u"blue");

{
    LikesProgram::Log::LoggerContextScope scope(u"tenant", u"green");
    scope.SetModule(u"inner");
    scope.SetTraceId(u"trace-inner");
    scope.SetContextField(u"operation", u"create");

    logger.Log(LikesProgram::Log::Level::Info, std::source_location::current(),
        u"inside scope");
}

logger.Log(LikesProgram::Log::Level::Info, std::source_location::current(),
    u"outside scope");
```

离开作用域后会恢复构造 `LoggerContextScope` 前的线程上下文。

## 文本格式

默认输出格式是 `Text`：

```cpp
LikesProgram::Log::LoggerOptions options;
options.outputFormat = LikesProgram::Log::LogOutputFormat::Text;
logger.Configure(options);
```

文本日志形态大致为：

```text
[2026-06-29 10:20:30.123] [T:worker-a] [ConsoleSink] [Info] [Logger:orders] [Module:checkout] [TraceId:trace-1] [Context.tenant:blue] [Function:main] (main.cpp:12) message
```

字段说明：

- 时间：本地时间，毫秒精度。
- `T`：线程展示名；未设置时输出底层 thread id。
- Sink 名称：如 `ConsoleSink`、`FileSink` 或自定义名称。
- Level：`Trace`/`Debug`/`Info`/`Warn`/`Error`/`Fatal`。
- Logger/Module/Category/TraceId/SpanId/RequestId：非空时才输出。
- 自定义字段：显示为 `[Context.<key>:<value>]`。
- Debug 开启时输出 `[Function:...] (file:line)`。

## JSON Lines 格式

开启 JSON Lines：

```cpp
LikesProgram::Log::LoggerOptions options;
options.outputFormat = LikesProgram::Log::LogOutputFormat::JsonLines;
logger.Configure(options);
```

输出是一行一个 JSON 对象：

```json
{"timestamp":"2026-06-29T10:20:30.123+0800","level":"Warn","logger":"orders","sink":"ConsoleSink","thread":"worker-a","thread_id":"1234","process_id":100,"module":"checkout","request_id":"req-42","message":"slow request 7","context":{"tenant":"blue"}}
```

说明：

- 这是日志场景的固定字段 writer，不是通用 JSON 库。
- 字符串使用 Core 的 `String::EscapeJson` 转义。
- 自定义字段放在 `context` 子对象中。
- 如果自定义字段名和内置字段同名，例如 `module`，它仍放在 `context.module`，不会覆盖顶层 `"module"`。
- 空字符串字段通常不输出。

## Message 结构

自定义 Sink 会收到 `Message`：

```cpp
struct Message {
    Level level;
    String msg;
    String file;
    int line;
    std::thread::id tid;
    String threadName;
    std::chrono::system_clock::time_point timestamp;
    String func;
    Level minLevel;
    String::Encoding encoding;
    bool debug;
    LogOutputFormat outputFormat;
    String loggerName;
    String module;
    String category;
    String traceId;
    String spanId;
    String requestId;
    uint64_t processId;
    std::vector<LogContextField> contextFields;
};
```

二开自定义 Sink 时一般只需要 `level`、`msg`、`timestamp`、`loggerName`、`module`、`traceId`、`contextFields`，或直接调用 `FormatLogMessage(message)`。

## Flush、Stats 和 Diagnostics

等待队列清空并刷新 Sink：

```cpp
bool drained = logger.Flush(std::chrono::seconds(5));
```

`Flush` 行为：

- 等待队列为空且没有正在写 Sink 的消息。
- 成功 drain 后调用所有 Sink 的 `Flush()`。
- Sink 的 `Flush()` 抛异常不会让 `Logger::Flush` 返回 false，但会计入 `sinkWriteFailures`。
- 等待超时返回 `false`，并计入 `flushTimeouts`。

运行统计：

```cpp
auto stats = logger.Stats();

stats.acceptedMessages;
stats.processedMessages;
stats.droppedMessages;
stats.enqueueTimeouts;
stats.sinkWriteFailures;
stats.flushTimeouts;
stats.currentQueueSize;
stats.queueHighWatermark;
stats.sinkCount;
stats.running;
```

配置和统计诊断：

```cpp
auto diagnostics = logger.Diagnostics();

diagnostics.options;
diagnostics.stats;
diagnostics.minLevel;
diagnostics.encoding;
diagnostics.loggerName;
diagnostics.debug;
```

导出诊断：

```cpp
auto text = logger.ExportDiagnostics();
auto json = logger.ExportDiagnostics(LikesProgram::Log::LogOutputFormat::JsonLines);
```

文本以 `LoggerDiagnostics{...}` 开头，适合人工排查。JSON Lines 形式会输出固定字段对象，适合机器采集。

## 推荐初始化模板

服务启动时：

```cpp
auto& logger = LikesProgram::Log::Logger::Instance(false, false);
logger.Shutdown(true); // 如果上一次测试或热重载留下状态，先清掉

LikesProgram::Log::LoggerOptions options;
options.maxQueueSize = 65536;
options.overflowPolicy = LikesProgram::Log::QueueOverflowPolicy::Block;
options.enqueueTimeout = std::chrono::milliseconds(100);
options.outputFormat = LikesProgram::Log::LogOutputFormat::Text;

logger.Configure(options);
logger.SetEncoding(LikesProgram::String::Encoding::UTF8);
logger.SetLevel(LikesProgram::Log::Level::Info);
logger.SetLoggerName(u"my-service");
logger.SetSinks({
    LikesProgram::Log::ConsoleSink::CreateSink(),
    LikesProgram::Log::FileSink::CreateSink(u"./logs", u"my-service.log")
});

logger.Start();
```

服务退出时：

```cpp
bool drained = logger.Shutdown(std::chrono::seconds(5), true);
if (!drained) {
    // 这里可以输出到 stderr 或上报退出异常
}
```

## 使用注意事项

`Logger::Instance()` 只获取单例，不启动后台线程。需要自动启动时使用 `Instance(true)`，或者获取后手动调用 `Start()`。

没有 Sink 时，日志不会出现在控制台或文件中；如果 Logger 已启动，这些日志仍会进入队列并计入统计。

使用 `LogInfo`、`LogWarn` 等宏时，也需要提前配置并启动全局 Logger。

`Log(...)` 是异步入队。程序退出前调用 `Flush()` 或 `Shutdown()`，可以等待队列中的日志写完。

`Shutdown()` 默认清空 Sink。需要保留 Sink 列表并稍后再次 `Start()` 时，使用 `Shutdown(false)`。

上下文是线程局部的。在线程池中复用线程时，推荐在任务范围内使用 `LoggerContextScope`，或在任务结束时主动 `ClearContext()`。

`FileSink` 的保留策略只管理符合当前 filename 命名规则的日志文件，其他文件会保留。

JSON Lines 是日志输出格式，适合日志采集；复杂对象序列化仍建议使用专门的 JSON 库。

## 更多可运行样例

- `examples/LoggingExample.cpp`：包身份、Logger 启停、控制台 Sink 基础用法。
- `tests/LoggingPackageTests.cpp`：可参考的行为样例，包含背压、异常隔离、上下文、JSON Lines、文件轮转、失败重试、多进程文件写入、LoggerConfig 和诊断。
- `benchmarks/LoggingBenchmark.cpp`：Release 场景下的日志开销观察样例。
