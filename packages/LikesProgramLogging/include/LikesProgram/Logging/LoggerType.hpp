#pragma once
#include <LikesProgram/Logging/system/LikesProgramLoggingExport.hpp>
#include <LikesProgram/Core/String.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

namespace LikesProgram {
    namespace Log {
        // 日志严重级别，数值越大表示越需要保留。
        enum class Level {
            Trace = 0,
            Debug,
            Info,
            Warn,
            Error,
            Fatal
        };

        // 将日志级别转换为稳定展示文本。
        LIKESPROGRAM_LOGGING_API const String LevelToString(Level level);

        // 将文本解析为日志级别，无法识别时返回 defaultLevel。
        LIKESPROGRAM_LOGGING_API Level StringToLevel(const String& levelString,
            const Level defaultLevel = Level::Trace);

        // 异步队列满时的背压策略。
        enum class QueueOverflowPolicy {
            Block = 0,
            DropNewest,
            DropOldest
        };

        // Sink 输出格式，Text 兼容旧日志，JsonLines 用于结构化采集。
        enum class LogOutputFormat {
            Text = 0,
            JsonLines
        };

        // 单个线程局部上下文字段快照。
        struct LogContextField {
            String key;       // 上下文字段名，空名会在格式化时忽略
            String value;     // 上下文字段值，写入时按当前输出格式转义
        };

        // Logger 运行选项，控制队列、背压和默认输出格式。
        struct LoggerOptions {
            size_t maxQueueSize = 65536;                                      // 异步队列上限，0 表示不限制
            QueueOverflowPolicy overflowPolicy = QueueOverflowPolicy::Block;  // 队列满时采用的背压策略
            std::chrono::milliseconds enqueueTimeout{ 100 };                  // Block 策略等待容量的最长时间
            LogOutputFormat outputFormat = LogOutputFormat::Text;             // Sink 默认输出格式，Text 兼容旧日志
        };

        // Logger 运行统计快照，所有计数为进程内累计值。
        struct LoggerStats {
            uint64_t acceptedMessages = 0;       // 已进入后台队列的日志条数
            uint64_t processedMessages = 0;      // 已完成 Sink 分发的日志条数
            uint64_t droppedMessages = 0;        // 因背压、停止或队列裁剪丢弃的日志条数
            uint64_t enqueueTimeouts = 0;        // Block 策略等待容量超时次数
            uint64_t sinkWriteFailures = 0;      // Sink::Write 或 Sink::Flush 抛错次数
            uint64_t sinkRetryScheduled = 0;     // Sink 写失败后已调度的重试次数
            uint64_t sinkRetryDropped = 0;       // 重试耗尽后放弃的次数
            uint64_t flushTimeouts = 0;          // Flush/Shutdown 等待 drain 超时次数
            size_t currentQueueSize = 0;         // 当前等待分发的队列长度
            size_t queueHighWatermark = 0;       // 进程内观测到的最高队列长度
            size_t retryQueueSize = 0;           // 当前等待重试的队列长度
            size_t retryQueueHighWatermark = 0;  // 进程内观测到的最高重试队列长度
            size_t sinkCount = 0;                // 当前注册的 Sink 数量
            bool running = false;                // 后台线程是否处于接收/分发状态
        };

        // Logger 自诊断快照，聚合配置、统计和当前输出策略。
        struct LoggerDiagnostics {
            LoggerOptions options;                         // 当前队列、背压和输出格式配置快照
            LoggerStats stats;                             // 当前运行时计数、水位和 Sink 数量快照
            Level minLevel = Level::Trace;                 // 当前全局日志过滤级别
            String::Encoding encoding = String::Encoding::UTF8; // 当前 Sink 输出编码
            String loggerName;                             // 当前 Logger 逻辑名称，可为空
            bool debug = false;                            // 当前是否在格式化时输出调用点信息
        };

        // 后台队列中传递的完整日志消息快照。
        struct Message {
            Level level = Level::Trace;                             // 本条日志级别
            String msg;                                              // 已格式化的日志正文
            String file;                                             // 调用点源文件名，可为空
            int line = 0;                                            // 调用点行号，未知时为 0
            std::thread::id tid;                                     // 写入日志的线程 id
            String threadName;                                       // 线程展示名，未设置时为空
            std::chrono::system_clock::time_point timestamp;         // 日志创建时间点
            String func;                                             // 调用点函数名，可为空
            Level minLevel = Level::Info;                            // 写入时的全局过滤级别
            String::Encoding encoding = String::Encoding::UTF8;      // Sink 输出编码策略
            bool debug = true;                                       // 是否输出调用点调试信息
            LogOutputFormat outputFormat = LogOutputFormat::Text;    // 本条日志使用的 Sink 格式快照
            String loggerName;                                       // Logger 逻辑名称，便于多组件区分
            String module;                                           // 模块名，通常对应业务或包边界
            String category;                                         // 类别名，通常对应日志主题
            String traceId;                                          // 调用链 trace id，未设置时为空
            String spanId;                                           // 调用链 span id，未设置时为空
            String requestId;                                        // 请求 id，未设置时为空
            uint64_t processId = 0;                                  // 创建日志的进程 id，未知时为 0
            std::vector<LogContextField> contextFields;              // 线程局部上下文字段快照
        };
    }
}
