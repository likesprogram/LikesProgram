#pragma once
#include <LikesProgram/Core/Result.hpp>
#include <LikesProgram/Core/Status.hpp>
#include <LikesProgram/Logging/sinks/ConsoleSink.hpp>
#include <LikesProgram/Logging/sinks/FileSink.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace LikesProgram {
    namespace Log {
        // 单个 Sink 写失败后的重试策略。
        struct RetryConfig {
            bool enabled = false;                         // 是否启用 Sink 写失败重试
            uint32_t maxAttempts = 0;                     // 首次写失败后的最大重试次数
            size_t maxQueueSize = 4096;                   // 当前 Sink 最多允许积压的重试任务数
            std::chrono::milliseconds initialBackoff{ 0 };// 第一次重试前等待时间
            std::chrono::milliseconds maxBackoff{ 0 };    // 指数退避最大等待时间，0 表示沿用 initialBackoff
        };

        // 开放式 Sink 配置，可包装任意用户自定义 Sink。
        struct SinkConfig {
            String name;                                      // 逻辑名称，主要用于有效配置导出和排障
            bool enabled = true;                              // false 时 ApplyConfig 会忽略该 Sink
            Level minLevel = Level::Trace;                    // 当前 Sink 的独立最低级别
            bool overrideOutputFormat = false;                // 是否覆盖 LoggerOptions 中的全局输出格式
            LogOutputFormat outputFormat = LogOutputFormat::Text; // 覆盖启用时使用的 Sink 输出格式
            RetryConfig retry;                                // 当前 Sink 写失败后的重试策略
            std::shared_ptr<Sink> sink;                       // 任意用户自定义 Sink 或内置 Sink
        };

        // 内置控制台 Sink 的便捷配置。
        struct ConsoleSinkConfig {
            String name = u"console";                         // 生成 SinkConfig 时使用的逻辑名称
            bool enabled = true;                              // false 时生成的 SinkConfig 不参与输出
            Level minLevel = Level::Trace;                    // 控制台 Sink 独立最低级别
            bool overrideOutputFormat = false;                // 是否覆盖全局输出格式
            LogOutputFormat outputFormat = LogOutputFormat::Text; // 控制台输出格式
            RetryConfig retry;                                // 控制台写失败后的重试策略
        };

        // 内置文件 Sink 的便捷配置。
        struct FileSinkConfig {
            String name = u"file";                            // 生成 SinkConfig 时使用的逻辑名称
            bool enabled = true;                              // false 时生成的 SinkConfig 不参与输出
            Level minLevel = Level::Trace;                    // 文件 Sink 独立最低级别
            bool overrideOutputFormat = false;                // 是否覆盖全局输出格式
            LogOutputFormat outputFormat = LogOutputFormat::Text; // 文件输出格式
            RetryConfig retry;                                // 文件写失败后的重试策略
            String path = u"./logs";                          // 日志根目录
            String filename = u"Logger.log";                  // 日志基础文件名
            FileSinkOptions fileOptions;                      // 文件轮转和保留策略
            MultiProcessFileConfig multiProcess;              // 多进程同文件写入策略
        };

        // Logger 的完整开放式配置模型。
        struct LoggerConfig {
            Level level = Level::Info;                        // 全局最低日志级别
            String loggerName;                                // Logger 逻辑名称
            String::Encoding encoding = String::Encoding::UTF8; // Sink 输出编码
            bool debug = false;                               // 是否输出 source_location
            LoggerOptions options;                            // 队列、背压和默认输出格式
            std::vector<SinkConfig> sinks;                    // 开放式 Sink 列表，可包含用户自定义 Sink
        };

        // 校验 RetryConfig 的退避边界，成功时返回 Ok。
        LIKESPROGRAM_LOGGING_API Status ValidateRetryConfig(const RetryConfig& config);

        // 校验完整 LoggerConfig，失败时返回首个明确错误。
        LIKESPROGRAM_LOGGING_API Status ValidateLoggerConfig(const LoggerConfig& config);

        // 使用内置 ConsoleSink 便捷构造开放式 SinkConfig。
        LIKESPROGRAM_LOGGING_API Result<SinkConfig> MakeConsoleSinkConfig(const ConsoleSinkConfig& config);

        // 使用内置 FileSink 便捷构造开放式 SinkConfig。
        LIKESPROGRAM_LOGGING_API Result<SinkConfig> MakeFileSinkConfig(const FileSinkConfig& config);
    }
}
