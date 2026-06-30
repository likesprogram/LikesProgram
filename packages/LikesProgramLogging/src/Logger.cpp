#include <LikesProgram/Logging/Logger.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <iostream>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace LikesProgram {
    namespace Log {
        namespace Detail {
            // 模板热路径共享的级别快照，默认 Trace 保持 Logger 构造前的开放语义。
            LIKESPROGRAM_LOGGING_API std::atomic<int> LoggerMinLevelSnapshot{ static_cast<int>(Level::Trace) };
        }

        namespace {
            // 日志级别以整数存入 atomic，避免跨线程读取 enum 时额外包装。
            constexpr int EncodeLevel(Level level) noexcept {
                return static_cast<int>(level);
            }

            // 外部输入可能来自旧配置或损坏快照，越界时回落到最宽松级别。
            constexpr Level DecodeLevel(int level) noexcept {
                if (level < EncodeLevel(Level::Trace) || level > EncodeLevel(Level::Fatal)) return Level::Trace;
                return static_cast<Level>(level);
            }

            // 编码枚举同样以整数存储，便于无锁读取当前 Sink 输出策略。
            constexpr int EncodeEncoding(String::Encoding encoding) noexcept {
                return static_cast<int>(encoding);
            }

            // 编码快照损坏时选择 UTF8，保证控制台和文件输出仍有稳定默认值。
            constexpr String::Encoding DecodeEncoding(int encoding) noexcept {
                if (encoding < EncodeEncoding(String::Encoding::GBK) ||
                    encoding > EncodeEncoding(String::Encoding::UTF32)) {
                    return String::Encoding::UTF8;
                }

                return static_cast<String::Encoding>(encoding);
            }

            // 输出格式只开放 Text 和 JsonLines，非法枚举统一收敛到 Text。
            LogOutputFormat NormalizeOutputFormat(LogOutputFormat format) noexcept {
                if (format == LogOutputFormat::JsonLines) return LogOutputFormat::JsonLines;
                return LogOutputFormat::Text;
            }

            // 背压策略在入口归一化，后续队列逻辑不再处理未知枚举分支。
            QueueOverflowPolicy NormalizeOverflowPolicy(QueueOverflowPolicy policy) noexcept {
                if (policy == QueueOverflowPolicy::DropNewest) return QueueOverflowPolicy::DropNewest;
                if (policy == QueueOverflowPolicy::DropOldest) return QueueOverflowPolicy::DropOldest;
                return QueueOverflowPolicy::Block;
            }

            // 对外配置进入运行期前先做轻量清洗，避免后台线程反复防御非法值。
            LoggerOptions NormalizeLoggerOptions(const LoggerOptions& options) {
                LoggerOptions normalized = options; // 统一收口非法枚举，确保运行期始终落到安全默认策略
                normalized.outputFormat = NormalizeOutputFormat(normalized.outputFormat);
                normalized.overflowPolicy = NormalizeOverflowPolicy(normalized.overflowPolicy);
                return normalized;
            }

            // 进程 id 写入每条消息快照，方便多进程同文件输出时排查来源。
            uint64_t CurrentProcessId() noexcept {
#ifdef _WIN32
                return static_cast<uint64_t>(_getpid());
#else
                return static_cast<uint64_t>(getpid());
#endif
            }

            // 诊断文本使用稳定英文枚举值，便于日志采集和脚本解析。
            String QueueOverflowPolicyToString(QueueOverflowPolicy policy) {
                switch (policy) {
                case QueueOverflowPolicy::Block:
                    return u"Block";
                case QueueOverflowPolicy::DropNewest:
                    return u"DropNewest";
                case QueueOverflowPolicy::DropOldest:
                    return u"DropOldest";
                default:
                    return u"Block";
                }
            }

            // 输出格式名与配置字段保持一致，避免诊断导出出现别名。
            String OutputFormatToString(LogOutputFormat format) {
                return NormalizeOutputFormat(format) == LogOutputFormat::JsonLines ? u"JsonLines" : u"Text";
            }

            // 编码名按公开枚举值输出，未知值同 DecodeEncoding 一样回退 UTF8。
            String EncodingToString(String::Encoding encoding) {
                switch (encoding) {
                case String::Encoding::GBK:
                    return u"GBK";
                case String::Encoding::UTF8:
                    return u"UTF8";
                case String::Encoding::UTF16:
                    return u"UTF16";
                case String::Encoding::UTF32:
                    return u"UTF32";
                default:
                    return u"UTF8";
                }
            }

            // JSON 和文本诊断都复用小写布尔字面量，保持机器可读。
            String BoolToString(bool value) {
                return value ? u"true" : u"false";
            }

            // 追加文本诊断字段时在调用处控制顺序，这里只负责分隔和拼接。
            void AppendDiagnosticsTextField(String& text, const String& name, const String& value, bool& first) {
                if (!first) text.Append(String(u", "));
                first = false;
                text.Append(name);
                text.Append(String(u"="));
                text.Append(value);
            }

            // JSON key 统一转义，避免 loggerName 等字段影响诊断结构。
            void AppendDiagnosticsJsonKey(String& text, const String& name, bool& first) {
                if (!first) text.Append(String(u","));
                first = false;
                text.Append(String(u"\""));
                text.Append(String::EscapeJson(name));
                text.Append(String(u"\":"));
            }

            // 字符串字段在最小 JSON writer 内完成转义和引号封装。
            void AppendDiagnosticsJsonString(String& text, const String& name, const String& value, bool& first) {
                AppendDiagnosticsJsonKey(text, name, first);
                text.Append(String(u"\""));
                text.Append(String::EscapeJson(value));
                text.Append(String(u"\""));
            }

            // 无符号计数值直接输出十进制，避免引号影响下游聚合。
            void AppendDiagnosticsJsonNumber(String& text, const String& name, uint64_t value, bool& first) {
                AppendDiagnosticsJsonKey(text, name, first);
                text.Append(String(value));
            }

            // 毫秒级超时可能为负值或零，保留有符号输出能力。
            void AppendDiagnosticsJsonSignedNumber(String& text, const String& name, int64_t value, bool& first) {
                AppendDiagnosticsJsonKey(text, name, first);
                text.Append(String(value));
            }

            // 布尔字段不加引号，保持 JSON Lines 诊断可直接被索引。
            void AppendDiagnosticsJsonBool(String& text, const String& name, bool value, bool& first) {
                AppendDiagnosticsJsonKey(text, name, first);
                text.Append(BoolToString(value));
            }

            // 文本诊断面向人工排障，固定顺序能让差异对比更稳定。
            String FormatDiagnosticsText(const LoggerDiagnostics& diagnostics) {
                String text(u"LoggerDiagnostics{"); // 单行文本便于健康检查和人工排障直接输出
                bool first = true; // 控制字段分隔符，首字段前不输出逗号

                // 配置字段放在前面，先说明 Logger 当前运行策略。
                AppendDiagnosticsTextField(text, u"running", BoolToString(diagnostics.stats.running), first);
                AppendDiagnosticsTextField(text, u"level", LevelToString(diagnostics.minLevel), first);
                AppendDiagnosticsTextField(text, u"encoding", EncodingToString(diagnostics.encoding), first);
                AppendDiagnosticsTextField(text, u"output_format", OutputFormatToString(diagnostics.options.outputFormat), first);
                AppendDiagnosticsTextField(text, u"overflow_policy",
                    QueueOverflowPolicyToString(diagnostics.options.overflowPolicy), first);
                AppendDiagnosticsTextField(text, u"max_queue_size",
                    String(static_cast<uint64_t>(diagnostics.options.maxQueueSize)), first);
                AppendDiagnosticsTextField(text, u"enqueue_timeout_ms",
                    String(static_cast<int64_t>(diagnostics.options.enqueueTimeout.count())), first);
                AppendDiagnosticsTextField(text, u"logger", diagnostics.loggerName, first);
                AppendDiagnosticsTextField(text, u"debug", BoolToString(diagnostics.debug), first);

                // 运行计数字段跟在配置后面，方便人工从左到右扫状态。
                AppendDiagnosticsTextField(text, u"accepted_messages",
                    String(diagnostics.stats.acceptedMessages), first);
                AppendDiagnosticsTextField(text, u"processed_messages",
                    String(diagnostics.stats.processedMessages), first);
                AppendDiagnosticsTextField(text, u"dropped_messages",
                    String(diagnostics.stats.droppedMessages), first);
                AppendDiagnosticsTextField(text, u"enqueue_timeouts",
                    String(diagnostics.stats.enqueueTimeouts), first);
                AppendDiagnosticsTextField(text, u"sink_write_failures",
                    String(diagnostics.stats.sinkWriteFailures), first);
                AppendDiagnosticsTextField(text, u"sink_retry_scheduled",
                    String(diagnostics.stats.sinkRetryScheduled), first);
                AppendDiagnosticsTextField(text, u"sink_retry_dropped",
                    String(diagnostics.stats.sinkRetryDropped), first);
                AppendDiagnosticsTextField(text, u"flush_timeouts",
                    String(diagnostics.stats.flushTimeouts), first);
                AppendDiagnosticsTextField(text, u"current_queue_size",
                    String(static_cast<uint64_t>(diagnostics.stats.currentQueueSize)), first);
                AppendDiagnosticsTextField(text, u"queue_high_watermark",
                    String(static_cast<uint64_t>(diagnostics.stats.queueHighWatermark)), first);
                AppendDiagnosticsTextField(text, u"retry_queue_size",
                    String(static_cast<uint64_t>(diagnostics.stats.retryQueueSize)), first);
                AppendDiagnosticsTextField(text, u"retry_queue_high_watermark",
                    String(static_cast<uint64_t>(diagnostics.stats.retryQueueHighWatermark)), first);
                AppendDiagnosticsTextField(text, u"sink_count",
                    String(static_cast<uint64_t>(diagnostics.stats.sinkCount)), first);

                text.Append(String(u"}"));
                return text;
            }

            // JSON Lines 诊断面向采集系统，字段集与文本诊断保持一一对应。
            String FormatDiagnosticsJson(const LoggerDiagnostics& diagnostics) {
                String text(u"{"); // JSON Lines 固定字段 writer，不依赖第三方 JSON 库
                bool first = true; // 控制 JSON 字段分隔符，首字段前不输出逗号

                // kind 字段固定为首字段，便于混合日志流中快速识别诊断事件。
                AppendDiagnosticsJsonString(text, u"kind", u"logger_diagnostics", first);
                AppendDiagnosticsJsonBool(text, u"running", diagnostics.stats.running, first);
                AppendDiagnosticsJsonString(text, u"level", LevelToString(diagnostics.minLevel), first);
                AppendDiagnosticsJsonString(text, u"encoding", EncodingToString(diagnostics.encoding), first);
                AppendDiagnosticsJsonString(text, u"output_format",
                    OutputFormatToString(diagnostics.options.outputFormat), first);
                AppendDiagnosticsJsonString(text, u"overflow_policy",
                    QueueOverflowPolicyToString(diagnostics.options.overflowPolicy), first);
                AppendDiagnosticsJsonNumber(text, u"max_queue_size",
                    static_cast<uint64_t>(diagnostics.options.maxQueueSize), first);
                AppendDiagnosticsJsonSignedNumber(text, u"enqueue_timeout_ms",
                    static_cast<int64_t>(diagnostics.options.enqueueTimeout.count()), first);
                AppendDiagnosticsJsonString(text, u"logger", diagnostics.loggerName, first);
                AppendDiagnosticsJsonBool(text, u"debug", diagnostics.debug, first);

                // 所有计数以数字写出，避免采集端再做字符串转型。
                AppendDiagnosticsJsonNumber(text, u"accepted_messages",
                    diagnostics.stats.acceptedMessages, first);
                AppendDiagnosticsJsonNumber(text, u"processed_messages",
                    diagnostics.stats.processedMessages, first);
                AppendDiagnosticsJsonNumber(text, u"dropped_messages",
                    diagnostics.stats.droppedMessages, first);
                AppendDiagnosticsJsonNumber(text, u"enqueue_timeouts",
                    diagnostics.stats.enqueueTimeouts, first);
                AppendDiagnosticsJsonNumber(text, u"sink_write_failures",
                    diagnostics.stats.sinkWriteFailures, first);
                AppendDiagnosticsJsonNumber(text, u"sink_retry_scheduled",
                    diagnostics.stats.sinkRetryScheduled, first);
                AppendDiagnosticsJsonNumber(text, u"sink_retry_dropped",
                    diagnostics.stats.sinkRetryDropped, first);
                AppendDiagnosticsJsonNumber(text, u"flush_timeouts",
                    diagnostics.stats.flushTimeouts, first);
                AppendDiagnosticsJsonNumber(text, u"current_queue_size",
                    static_cast<uint64_t>(diagnostics.stats.currentQueueSize), first);
                AppendDiagnosticsJsonNumber(text, u"queue_high_watermark",
                    static_cast<uint64_t>(diagnostics.stats.queueHighWatermark), first);
                AppendDiagnosticsJsonNumber(text, u"retry_queue_size",
                    static_cast<uint64_t>(diagnostics.stats.retryQueueSize), first);
                AppendDiagnosticsJsonNumber(text, u"retry_queue_high_watermark",
                    static_cast<uint64_t>(diagnostics.stats.retryQueueHighWatermark), first);
                AppendDiagnosticsJsonNumber(text, u"sink_count",
                    static_cast<uint64_t>(diagnostics.stats.sinkCount), first);

                text.Append(String(u"}"));
                return text;
            }

            struct ThreadContext {
                String threadName;                       // 当前线程展示名，空值表示回退到底层 id
                String module;                           // 当前线程模块上下文
                String category;                         // 当前线程类别上下文
                String traceId;                          // 当前线程 trace id
                String spanId;                           // 当前线程 span id
                String requestId;                        // 当前线程 request id
                std::vector<LogContextField> fields;     // 当前线程自定义上下文字段
            };

            thread_local ThreadContext localContext;      // 每个调用线程独立维护日志上下文

            // 消息创建时只复制展示名，不在后台线程访问调用线程的 thread_local。
            String CurrentThreadNameFallback() {
                return localContext.threadName;
            }

            // 同 key 覆盖旧值，保持上下文字段在单条消息中唯一。
            void SetLocalContextField(const String& key, const String& value) {
                if (key.Empty()) return;

                for (auto& field : localContext.fields) {
                    if (field.key == key) {
                        field.value = value;
                        return;
                    }
                }

                localContext.fields.push_back(LogContextField{ key, value });
            }

            // 删除字段时保留其他上下文顺序，方便后续消息稳定输出。
            void RemoveLocalContextField(const String& key) {
                if (key.Empty()) return;

                localContext.fields.erase(std::remove_if(localContext.fields.begin(),
                    localContext.fields.end(), [&key](const LogContextField& field) {
                        return field.key == key;
                    }), localContext.fields.end());
            }

            // Scope 构造时复制完整上下文，析构时可无锁恢复当前线程状态。
            ThreadContext CaptureLocalContext() {
                return localContext;
            }

            // 恢复只影响当前线程，不会触碰后台 worker 的执行上下文。
            void RestoreLocalContext(const ThreadContext& context) {
                localContext = context;
            }

            struct SinkRuntime {
                String name;                                      // 配置层逻辑名称，便于导出有效配置
                bool enabled = true;                              // false 时不参与分发
                Level minLevel = Level::Trace;                    // Sink 独立最低级别
                bool overrideOutputFormat = false;                // 是否覆盖消息的全局输出格式
                LogOutputFormat outputFormat = LogOutputFormat::Text; // 覆盖启用时使用的输出格式
                RetryConfig retry;                                // 写失败后的重试策略
                std::shared_ptr<Sink> sink;                       // 真实输出目标，可为用户自定义 Sink
            };

            struct RetryTask {
                SinkRuntime runtime;                              // 失败时的 Sink 配置快照
                Message message;                                  // 需要重试写入的日志消息快照
                uint32_t attempt = 1;                             // 当前是第几次重试
                std::chrono::milliseconds delay{ 0 };             // 本次重试前等待时间
            };

            bool TryPushRetryTaskLocked(std::deque<RetryTask>& queue,
                const std::vector<std::shared_ptr<Sink>>& inFlightSinks, RetryTask&& task,
                bool replacingCurrent = false) {
                const size_t maxQueueSize = task.runtime.retry.maxQueueSize; // 单个 Sink 允许积压的重试任务上限
                if (maxQueueSize == 0 || !task.runtime.sink) return false;

                size_t queuedForSink = 0; // 按 Sink 实例限制积压，避免一个故障 Sink 吃光内存
                for (const auto& pending : queue) {
                    if (pending.runtime.sink == task.runtime.sink) ++queuedForSink;
                }
                bool skippedCurrent = false; // 当前 retry 失败后重排队时，不把自身算成新增积压
                for (const auto& sink : inFlightSinks) {
                    if (replacingCurrent && !skippedCurrent && sink == task.runtime.sink) {
                        skippedCurrent = true;
                        continue;
                    }
                    if (sink == task.runtime.sink) ++queuedForSink;
                }

                if (queuedForSink >= maxQueueSize) return false;
                queue.push_back(std::move(task));
                return true;
            }

            // 高水位只在持有重试锁时更新，调用方负责保证 queue 稳定。
            void RecordRetryQueueHighWatermark(const std::deque<RetryTask>& queue, size_t& highWatermark) {
                highWatermark = (std::max)(highWatermark, queue.size());
            }

            // 将旧 AddSink(shared_ptr<Sink>) 入口包装成默认运行时配置。
            SinkRuntime RuntimeFromSink(std::shared_ptr<Sink> sink) {
                SinkRuntime runtime; // 默认级别和格式继承 Logger 全局配置
                runtime.sink = std::move(sink);
                return runtime;
            }

            // 将配置层 SinkConfig 展开为写线程可直接读取的运行时快照。
            SinkRuntime RuntimeFromConfig(const SinkConfig& config) {
                SinkRuntime runtime; // 当前 Sink 的运行时调度、格式和重试配置
                runtime.name = config.name;
                runtime.enabled = config.enabled;
                runtime.minLevel = config.minLevel;
                runtime.overrideOutputFormat = config.overrideOutputFormat;
                runtime.outputFormat = NormalizeOutputFormat(config.outputFormat);
                runtime.retry = config.retry;
                runtime.sink = config.sink;
                return runtime;
            }

            // 将运行时快照还原为配置对象，供 EffectiveConfig 导出。
            SinkConfig ConfigFromRuntime(const SinkRuntime& runtime) {
                SinkConfig config; // 输出给调用方的 Sink 配置快照
                config.name = runtime.name;
                config.enabled = runtime.enabled;
                config.minLevel = runtime.minLevel;
                config.overrideOutputFormat = runtime.overrideOutputFormat;
                config.outputFormat = runtime.outputFormat;
                config.retry = runtime.retry;
                config.sink = runtime.sink;
                return config;
            }

            std::chrono::milliseconds NextRetryDelay(const RetryConfig& retry,
                std::chrono::milliseconds previous) {
                if (!retry.enabled) return std::chrono::milliseconds(0);

                std::chrono::milliseconds base = retry.initialBackoff; // 当前轮次准备使用的退避间隔
                std::chrono::milliseconds maxDelay = retry.maxBackoff.count() > 0
                    ? retry.maxBackoff
                    : retry.initialBackoff;
                // 已有间隔按指数退避翻倍，溢出时钳制到 chrono 可表达的最大值。
                if (previous.count() > 0) {
                    const auto maxCount = std::chrono::milliseconds::max().count();
                    base = previous.count() > maxCount / 2
                        ? std::chrono::milliseconds::max()
                        : previous + previous;
                }

                if (maxDelay.count() > 0 && base > maxDelay) return maxDelay;
                return base;
            }
        }

        struct Logger::LoggerImpl {
            // 运行状态和配置使用 atomic，前台写日志路径可以快速读取快照。
            std::atomic<bool> m_stop{ true };                       // true 表示后台线程已停止或正在停止
            std::atomic<int> m_minLevel{ EncodeLevel(Level::Trace) };// 当前全局过滤级别
            std::atomic<int> m_encoding{ EncodeEncoding(String::Encoding::UTF8) }; // Sink 输出编码策略
            std::atomic<bool> m_debug{ false };                     // 是否输出调用点调试信息

            // Sink 列表读多写少，用 shared_mutex 缩短正常分发路径的独占时间。
            mutable std::shared_mutex m_sinkMtx;                    // 保护 m_sinks 的读写
            std::vector<SinkRuntime> m_sinks;                        // 已注册输出目标列表和分发策略

            // 主队列锁同时保护配置快照，确保入队时消息携带一致的输出策略。
            mutable std::mutex m_queueMtx;                          // 保护队列、配置快照和 drain 状态
            std::condition_variable m_cv;                           // 后台线程等待新消息或停止信号
            std::condition_variable m_capacityCv;                    // 前台线程等待队列容量
            std::condition_variable m_drainCv;                       // Flush/Shutdown 等待队列清空
            std::deque<Message> m_queue;                             // 待分发日志队列，支持丢弃最旧
            size_t m_inFlight = 0;                                   // 已出队但仍在写 Sink 的消息数
            LoggerOptions m_options;                                 // 队列容量和背压策略配置
            String m_loggerName;                                     // Logger 逻辑名称配置快照来源
            size_t m_queueHighWatermark = 0;                         // 队列最高水位

            // startMutex 串行化 Start/Shutdown，避免重复 join 或同时创建 worker。
            std::mutex m_startMutex;                                 // 保护后台线程创建和 join 状态
            std::thread m_worker;                                    // 后台分发线程
            std::thread m_retryWorker;                               // 后台失败重试线程

            // 重试队列独立于主队列，故障 Sink 不阻塞普通消息继续分发。
            mutable std::mutex m_retryMtx;                           // 保护重试队列和重试 in-flight 状态
            std::condition_variable m_retryCv;                       // 重试线程等待新任务或停止信号
            std::condition_variable m_retryDrainCv;                  // Flush/Shutdown 等待重试队列清空
            std::deque<RetryTask> m_retryQueue;                      // Sink 写失败后的延迟重试任务
            std::vector<std::shared_ptr<Sink>> m_retryInFlightSinks;  // 当前 retry worker 正在处理的 Sink
            size_t m_retryInFlight = 0;                              // 已出队但尚未完成的重试任务数
            size_t m_retryQueueHighWatermark = 0;                    // 重试队列最高水位

            // 统计全部使用原子累加，Stats 只需要读取近似一致的运行快照。
            std::atomic<uint64_t> m_acceptedMessages{ 0 };           // 已入队消息数
            std::atomic<uint64_t> m_processedMessages{ 0 };          // 已完成分发消息数
            std::atomic<uint64_t> m_droppedMessages{ 0 };            // 已丢弃消息数
            std::atomic<uint64_t> m_enqueueTimeouts{ 0 };            // 入队等待容量超时次数
            std::atomic<uint64_t> m_sinkWriteFailures{ 0 };          // Sink 写入或 Flush 失败次数
            std::atomic<uint64_t> m_flushTimeouts{ 0 };              // Flush/Shutdown 超时次数

            std::atomic<uint64_t> m_retryScheduled{ 0 };             // 已调度的 Sink 写重试次数
            std::atomic<uint64_t> m_retryDropped{ 0 };               // 因重试队列不可用而放弃的次数
        };

        Logger& Logger::Instance() {
            return InstanceInternal(false, false, false);
        }

        Logger& Logger::Instance(bool autoStart) {
            return InstanceInternal(autoStart, false, false);
        }

        Logger& Logger::Instance(bool autoStart, bool debug) {
            return InstanceInternal(autoStart, debug, true);
        }

        Logger& Logger::InstanceInternal(bool autoStart, bool debug, bool updateDebug) {
            static std::atomic<Logger*> instance{ nullptr }; // 进程内 Logger 单例指针
            static std::mutex mutex;                         // 保护首次创建过程

            Logger* current = instance.load(std::memory_order_acquire);
            if (!current) {
                std::lock_guard<std::mutex> lock(mutex);
                current = instance.load(std::memory_order_relaxed);
                if (!current) {
                    current = new Logger(autoStart, debug);
                    instance.store(current, std::memory_order_release);
                }
            }

            if (updateDebug && current->m_impl) {
                current->m_impl->m_debug.store(debug, std::memory_order_release);
            }

            if (current->m_impl && current->m_impl->m_stop.load(std::memory_order_acquire) && autoStart) {
                current->Start();
            }

            return *current;
        }

        Logger::Logger(bool autoStart, bool debug) : m_impl(new LoggerImpl{}) {
            m_impl->m_debug.store(debug, std::memory_order_release);
            m_impl->m_stop.store(true, std::memory_order_release);
            if (autoStart) Start();
        }

        Logger::~Logger() {
            if (!m_impl) return;

            Shutdown(std::chrono::milliseconds::max(), true);

            delete m_impl;
            m_impl = nullptr;
        }

        void Logger::ProcessLoop() {
            LoggerImpl* impl = m_impl; // 工作线程启动时的实现对象，析构会先 Shutdown 并等待线程退出。
            if (!impl) return;

            while (true) {
                Message message; // 从主队列取出的消息快照，离开队列锁后再写 Sink
                bool hasMessage = false; // false 表示 stop 且队列已空，worker 可以退出
                {
                    std::unique_lock<std::mutex> lock(impl->m_queueMtx);
                    // 等到有消息或收到停止信号；停止后仍会继续 drain 队列内剩余消息。
                    impl->m_cv.wait(lock, [impl] {
                        return !impl->m_queue.empty() || impl->m_stop.load(std::memory_order_acquire);
                    });

                    // 只要队列里还有消息就继续处理，避免 Shutdown 丢失已接受日志。
                    if (!impl->m_stop.load(std::memory_order_acquire) || !impl->m_queue.empty()) {
                        message = std::move(impl->m_queue.front());
                        impl->m_queue.pop_front();
                        ++impl->m_inFlight;
                        hasMessage = true;
                    }
                    // 显式释放锁，满足 MSVC 并发分析对退出路径的锁状态推断。
                    lock.unlock();
                }

                if (!hasMessage) break;
                impl->m_capacityCv.notify_all();

                std::vector<SinkRuntime> sinks; // 当前循环使用的 Sink 运行时快照
                {
                    // Sink 快照复制后再执行 Write，避免用户 Sink 回调阻塞配置更新。
                    std::shared_lock<std::shared_mutex> sinkLock(impl->m_sinkMtx);
                    sinks = impl->m_sinks;
                }

                for (const auto& runtime : sinks) {
                    // 每个 Sink 可独立禁用或设置更高最低级别，跳过不应接收的目标。
                    if (!runtime.enabled || !runtime.sink || message.level < runtime.minLevel) continue;

                    Message sinkMessage = message; // 当前 Sink 可覆盖输出格式，不能污染其他 Sink
                    if (runtime.overrideOutputFormat) {
                        sinkMessage.outputFormat = NormalizeOutputFormat(runtime.outputFormat);
                    }

                    try {
                        runtime.sink->Write(sinkMessage);
                    }
                    catch (const std::exception& ex) {
                        impl->m_sinkWriteFailures.fetch_add(1, std::memory_order_relaxed);
                        std::cerr << "[Logger Error] Sink write failed: " << ex.what() << std::endl;
                        // 停止流程中不再创建新重试，避免 Shutdown 被新任务拖住。
                        if (!impl->m_stop.load(std::memory_order_acquire) &&
                            runtime.retry.enabled && runtime.retry.maxAttempts > 0) {
                            RetryTask task; // 首次失败转入 retry worker 的任务快照
                            task.runtime = runtime;
                            task.message = std::move(sinkMessage);
                            task.attempt = 1;
                            task.delay = runtime.retry.initialBackoff;
                            bool queued = false; // 是否成功进入对应 Sink 的重试积压额度
                            {
                                std::lock_guard<std::mutex> retryLock(impl->m_retryMtx);
                                queued = TryPushRetryTaskLocked(impl->m_retryQueue,
                                    impl->m_retryInFlightSinks, std::move(task));
                                if (queued) {
                                    RecordRetryQueueHighWatermark(impl->m_retryQueue,
                                        impl->m_retryQueueHighWatermark);
                                }
                            }
                            if (queued) {
                                impl->m_retryScheduled.fetch_add(1, std::memory_order_relaxed);
                                impl->m_retryCv.notify_one();
                            }
                            else {
                                impl->m_retryDropped.fetch_add(1, std::memory_order_relaxed);
                            }
                        }
                    }
                    catch (...) {
                        impl->m_sinkWriteFailures.fetch_add(1, std::memory_order_relaxed);
                        std::cerr << "[Logger Error] Sink write failed: Unknown exception" << std::endl;
                        // 未知异常同样按配置重试，但不让异常穿透后台线程。
                        if (!impl->m_stop.load(std::memory_order_acquire) &&
                            runtime.retry.enabled && runtime.retry.maxAttempts > 0) {
                            RetryTask task; // 保留失败时的 Sink 与消息快照供异步重试
                            task.runtime = runtime;
                            task.message = std::move(sinkMessage);
                            task.attempt = 1;
                            task.delay = runtime.retry.initialBackoff;
                            bool queued = false; // false 时统计为重试丢弃，避免无界增长
                            {
                                std::lock_guard<std::mutex> retryLock(impl->m_retryMtx);
                                queued = TryPushRetryTaskLocked(impl->m_retryQueue,
                                    impl->m_retryInFlightSinks, std::move(task));
                                if (queued) {
                                    RecordRetryQueueHighWatermark(impl->m_retryQueue,
                                        impl->m_retryQueueHighWatermark);
                                }
                            }
                            if (queued) {
                                impl->m_retryScheduled.fetch_add(1, std::memory_order_relaxed);
                                impl->m_retryCv.notify_one();
                            }
                            else {
                                impl->m_retryDropped.fetch_add(1, std::memory_order_relaxed);
                            }
                        }
                    }
                }

                impl->m_processedMessages.fetch_add(1, std::memory_order_relaxed);

                {
                    std::unique_lock<std::mutex> lock(impl->m_queueMtx);
                    // in-flight 与队列共同决定 Flush 是否完成，必须在同一把锁下更新。
                    if (impl->m_inFlight > 0) --impl->m_inFlight;
                    lock.unlock();
                }
                impl->m_drainCv.notify_all();
            }

            // 退出时唤醒所有等待者，覆盖 stop 时队列为空但无人再通知的路径。
            impl->m_drainCv.notify_all();
            impl->m_capacityCv.notify_all();
        }

        void Logger::RetryLoop() {
            LoggerImpl* impl = m_impl; // 工作线程启动时的实现对象，析构会先 Shutdown 并等待线程退出。
            if (!impl) return;

            while (true) {
                RetryTask task; // 当前重试任务，离开重试锁后执行用户 Sink
                bool hasTask = false; // false 表示 stop 且重试队列已空
                {
                    std::unique_lock<std::mutex> lock(impl->m_retryMtx);
                    // stop 后只在重试队列清空时退出，保证已排队任务能被 drain。
                    impl->m_retryCv.wait(lock, [impl] {
                        return !impl->m_retryQueue.empty() ||
                            (impl->m_stop.load(std::memory_order_acquire) && impl->m_retryQueue.empty());
                    });

                    // 有任务时登记 in-flight Sink，用于限制同一故障 Sink 的积压。
                    if (!impl->m_stop.load(std::memory_order_acquire) || !impl->m_retryQueue.empty()) {
                        task = std::move(impl->m_retryQueue.front());
                        impl->m_retryQueue.pop_front();
                        ++impl->m_retryInFlight;
                        impl->m_retryInFlightSinks.push_back(task.runtime.sink);
                        hasTask = true;
                    }
                    // 显式释放锁后再 break/延迟等待，消除 C26115 的误判路径。
                    lock.unlock();
                }

                if (!hasTask) break;

                if (task.delay.count() > 0) {
                    std::unique_lock<std::mutex> delayLock(impl->m_retryMtx);
                    // 延迟可被 Shutdown 打断，避免析构在长退避时间上无谓等待。
                    impl->m_retryCv.wait_for(delayLock, task.delay, [impl] {
                        return impl->m_stop.load(std::memory_order_acquire);
                    });
                    delayLock.unlock();
                }

                const bool stopped = impl->m_stop.load(std::memory_order_acquire);
                bool success = false;
                if (!stopped) {
                    try {
                        // 重试阶段仍防御空 Sink，配置替换或用户错误不应终止线程。
                        if (task.runtime.sink) {
                            task.runtime.sink->Write(task.message);
                            success = true;
                        }
                    }
                    catch (const std::exception& ex) {
                        impl->m_sinkWriteFailures.fetch_add(1, std::memory_order_relaxed);
                        std::cerr << "[Logger Error] Sink retry failed: " << ex.what() << std::endl;
                    }
                    catch (...) {
                        impl->m_sinkWriteFailures.fetch_add(1, std::memory_order_relaxed);
                        std::cerr << "[Logger Error] Sink retry failed: Unknown exception" << std::endl;
                    }
                }

                if (!success && !stopped &&
                    task.runtime.retry.enabled &&
                    task.attempt < task.runtime.retry.maxAttempts) {
                    RetryTask next = task; // 下一轮重试继承当前消息和 Sink 快照
                    ++next.attempt;
                    next.delay = NextRetryDelay(task.runtime.retry, task.delay);
                    bool queued = false; // 重新入队失败会转为丢弃统计
                    {
                        std::unique_lock<std::mutex> lock(impl->m_retryMtx);
                        queued = TryPushRetryTaskLocked(impl->m_retryQueue,
                            impl->m_retryInFlightSinks, std::move(next), true);
                        if (queued) {
                            RecordRetryQueueHighWatermark(impl->m_retryQueue,
                                impl->m_retryQueueHighWatermark);
                        }
                        lock.unlock();
                    }
                    if (queued) {
                        impl->m_retryScheduled.fetch_add(1, std::memory_order_relaxed);
                        impl->m_retryCv.notify_one();
                    }
                    else {
                        impl->m_retryDropped.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                else if (!success && !stopped) {
                    impl->m_retryDropped.fetch_add(1, std::memory_order_relaxed);
                }

                {
                    std::unique_lock<std::mutex> lock(impl->m_retryMtx);
                    // 完成当前任务后同步清理 in-flight 状态，Flush 依赖该状态归零。
                    if (impl->m_retryInFlight > 0) --impl->m_retryInFlight;
                    auto current = std::find(impl->m_retryInFlightSinks.begin(),
                        impl->m_retryInFlightSinks.end(), task.runtime.sink);
                    if (current != impl->m_retryInFlightSinks.end()) {
                        impl->m_retryInFlightSinks.erase(current);
                    }
                    lock.unlock();
                }
                impl->m_retryDrainCv.notify_all();
            }

            // 线程退出时确保等待 retry drain 的 Flush/Shutdown 不会挂起。
            impl->m_retryDrainCv.notify_all();
        }

        void Logger::SetLevel(Level level) {
            if (!m_impl) return;

            // 级别热更新只影响后续入队消息，已入队消息保留创建时快照。
            m_impl->m_minLevel.store(EncodeLevel(level), std::memory_order_release);
            Detail::LoggerMinLevelSnapshot.store(EncodeLevel(level), std::memory_order_relaxed);
        }

        void Logger::SetEncoding(String::Encoding encoding) {
            if (!m_impl) return;

            // 编码策略在消息创建时固化，避免后台写入期间配置漂移。
            m_impl->m_encoding.store(EncodeEncoding(encoding), std::memory_order_release);
        }

        void Logger::Configure(const LoggerOptions& options) {
            if (!m_impl) return;

            std::lock_guard<std::mutex> lock(m_impl->m_queueMtx);
            // 队列策略与 loggerName 共用主队列锁，保证入队快照一致。
            m_impl->m_options = NormalizeLoggerOptions(options);
            m_impl->m_capacityCv.notify_all();
        }

        LoggerOptions Logger::Options() const {
            if (!m_impl) return LoggerOptions();

            std::lock_guard<std::mutex> lock(m_impl->m_queueMtx);
            // 返回配置副本，调用方不能直接观察内部可变状态。
            return m_impl->m_options;
        }

        LoggerStats Logger::Stats() const {
            LoggerStats stats; // 对外返回的统计快照，字段允许近似同一时刻
            if (!m_impl) return stats;

            // 原子计数先读取，避免在持锁期间做不必要工作。
            stats.acceptedMessages = m_impl->m_acceptedMessages.load(std::memory_order_relaxed);
            stats.processedMessages = m_impl->m_processedMessages.load(std::memory_order_relaxed);
            stats.droppedMessages = m_impl->m_droppedMessages.load(std::memory_order_relaxed);
            stats.enqueueTimeouts = m_impl->m_enqueueTimeouts.load(std::memory_order_relaxed);
            stats.sinkWriteFailures = m_impl->m_sinkWriteFailures.load(std::memory_order_relaxed);
            stats.sinkRetryScheduled = m_impl->m_retryScheduled.load(std::memory_order_relaxed);
            stats.sinkRetryDropped = m_impl->m_retryDropped.load(std::memory_order_relaxed);
            stats.flushTimeouts = m_impl->m_flushTimeouts.load(std::memory_order_relaxed);
            stats.running = !m_impl->m_stop.load(std::memory_order_acquire);

            {
                std::lock_guard<std::mutex> lock(m_impl->m_queueMtx);
                // 队列大小与高水位必须在主队列锁下成对读取。
                stats.currentQueueSize = m_impl->m_queue.size();
                stats.queueHighWatermark = m_impl->m_queueHighWatermark;
            }

            {
                std::lock_guard<std::mutex> lock(m_impl->m_retryMtx);
                // 重试队列独立锁保护，避免与主队列统计互相阻塞。
                stats.retryQueueSize = m_impl->m_retryQueue.size();
                stats.retryQueueHighWatermark = m_impl->m_retryQueueHighWatermark;
            }

            {
                std::shared_lock<std::shared_mutex> sinkLock(m_impl->m_sinkMtx);
                // Sink 数量读取共享锁即可，保持 AddSink/SetSinks 的互斥边界。
                stats.sinkCount = m_impl->m_sinks.size();
            }

            return stats;
        }

        LoggerDiagnostics Logger::Diagnostics() const {
            LoggerDiagnostics diagnostics; // 聚合配置、级别和统计的自诊断快照
            if (!m_impl) return diagnostics;

            // 原子配置字段无需持锁，主队列配置另行在锁内复制。
            diagnostics.minLevel = DecodeLevel(m_impl->m_minLevel.load(std::memory_order_acquire));
            diagnostics.encoding = DecodeEncoding(m_impl->m_encoding.load(std::memory_order_acquire));
            diagnostics.debug = m_impl->m_debug.load(std::memory_order_acquire);
            diagnostics.stats = Stats();

            {
                std::lock_guard<std::mutex> lock(m_impl->m_queueMtx);
                // 这两个字段与入队消息快照同锁读取，避免导出半更新配置。
                diagnostics.options = m_impl->m_options;
                diagnostics.loggerName = m_impl->m_loggerName;
            }

            return diagnostics;
        }

        String Logger::ExportDiagnostics(LogOutputFormat format) const {
            LoggerDiagnostics diagnostics = Diagnostics(); // 先取快照，再按请求格式序列化
            if (NormalizeOutputFormat(format) == LogOutputFormat::JsonLines) {
                return FormatDiagnosticsJson(diagnostics);
            }

            return FormatDiagnosticsText(diagnostics);
        }

        Status Logger::ApplyConfig(const LoggerConfig& config) {
            if (!m_impl) return Status::Internal(u"Logger is not initialized");

            // 校验失败时保持当前配置不变，避免半应用导致后台线程状态不一致。
            Status status = ValidateLoggerConfig(config);
            if (!status.IsOk()) return status;

            std::vector<SinkRuntime> runtimes; // 新 Sink 快照先在锁外构建，缩短替换锁时间
            runtimes.reserve(config.sinks.size());
            for (const auto& sink : config.sinks) {
                if (sink.enabled) runtimes.push_back(RuntimeFromConfig(sink));
            }

            {
                std::lock_guard<std::mutex> lock(m_impl->m_queueMtx);
                // 队列配置和 loggerName 一起切换，后续消息会拿到同一版本快照。
                m_impl->m_options = NormalizeLoggerOptions(config.options);
                m_impl->m_loggerName = config.loggerName;
                m_impl->m_capacityCv.notify_all();
            }

            // 原子字段可在锁外更新，读者总能看到某个有效枚举快照。
            m_impl->m_minLevel.store(EncodeLevel(config.level), std::memory_order_release);
            Detail::LoggerMinLevelSnapshot.store(EncodeLevel(config.level), std::memory_order_relaxed);
            m_impl->m_encoding.store(EncodeEncoding(config.encoding), std::memory_order_release);
            m_impl->m_debug.store(config.debug, std::memory_order_release);

            {
                std::unique_lock<std::shared_mutex> sinkLock(m_impl->m_sinkMtx);
                // Sink 列表一次性替换，正在分发的线程继续使用自己的旧快照。
                m_impl->m_sinks = std::move(runtimes);
            }

            return Status::OkStatus();
        }

        Result<LoggerConfig> Logger::EffectiveConfig() const {
            if (!m_impl) return Status::Internal(u"Logger is not initialized");

            LoggerConfig config; // 对外导出的当前有效配置副本
            // 原子字段先读出，主队列和 Sink 字段随后按各自锁复制。
            config.level = DecodeLevel(m_impl->m_minLevel.load(std::memory_order_acquire));
            config.encoding = DecodeEncoding(m_impl->m_encoding.load(std::memory_order_acquire));
            config.debug = m_impl->m_debug.load(std::memory_order_acquire);

            {
                std::lock_guard<std::mutex> lock(m_impl->m_queueMtx);
                // options/loggerName 与入队快照共用锁，导出时保持字段组合一致。
                config.options = m_impl->m_options;
                config.loggerName = m_impl->m_loggerName;
            }

            {
                std::shared_lock<std::shared_mutex> sinkLock(m_impl->m_sinkMtx);
                // 导出 Sink 时还原配置结构，不暴露后台运行时私有类型。
                config.sinks.reserve(m_impl->m_sinks.size());
                for (const auto& runtime : m_impl->m_sinks) {
                    config.sinks.push_back(ConfigFromRuntime(runtime));
                }
            }

            return config;
        }

        void Logger::AddSink(std::shared_ptr<Sink> sink) {
            if (!m_impl || !sink) return;

            std::unique_lock<std::shared_mutex> lock(m_impl->m_sinkMtx);
            // 旧入口没有独立配置，默认继承 Logger 全局级别和输出格式。
            m_impl->m_sinks.push_back(RuntimeFromSink(std::move(sink)));
        }

        void Logger::SetSinks(const std::vector<std::shared_ptr<Sink>>& sinks) {
            if (!m_impl) return;

            std::vector<SinkRuntime> filtered; // 新列表先在锁外构建，缩短独占锁时间
            filtered.reserve(sinks.size());
            for (const auto& sink : sinks) {
                if (sink) filtered.push_back(RuntimeFromSink(sink));
            }

            std::unique_lock<std::shared_mutex> lock(m_impl->m_sinkMtx);
            // 替换以移动完成，正在分发的 worker 不受新列表生命周期影响。
            m_impl->m_sinks = std::move(filtered);
        }

        void Logger::ClearSinks() {
            if (!m_impl) return;

            std::unique_lock<std::shared_mutex> lock(m_impl->m_sinkMtx);
            // 清空后日志仍会计入队列统计，只是不再分发到任何 Sink。
            m_impl->m_sinks.clear();
        }

        void Logger::SetLoggerName(const String& name) {
            if (!m_impl) return;

            std::lock_guard<std::mutex> lock(m_impl->m_queueMtx);
            // 名称随消息入队固化，已入队消息保留旧名称。
            m_impl->m_loggerName = name;
        }

        void Logger::SetThreadName(const String& name) {
            // thread_local 上下文只影响当前调用线程后续创建的消息。
            localContext.threadName = name;
        }

        void Logger::ClearThreadName() {
            // 清空后格式化阶段回退到底层 thread::id。
            localContext.threadName.Clear();
        }

        void Logger::SetContextField(const String& key, const String& value) {
            // 空 key 在底层被忽略，避免生成不可识别的上下文字段。
            SetLocalContextField(key, value);
        }

        void Logger::RemoveContextField(const String& key) {
            // 删除只影响当前线程上下文，不会改写已创建消息。
            RemoveLocalContextField(key);
        }

        void Logger::ClearContext() {
            // 保留 threadName，只清空业务追踪字段和自定义字段。
            localContext.module.Clear();
            localContext.category.Clear();
            localContext.traceId.Clear();
            localContext.spanId.Clear();
            localContext.requestId.Clear();
            localContext.fields.clear();
        }

        void Logger::SetModule(const String& module) {
            // 模块字段通常标识业务边界，作为消息快照随入队复制。
            localContext.module = module;
        }

        void Logger::SetCategory(const String& category) {
            // 类别字段用于同一模块内进一步分组日志主题。
            localContext.category = category;
        }

        void Logger::SetTraceId(const String& traceId) {
            // traceId 与 spanId 分离，方便调用链系统按需聚合。
            localContext.traceId = traceId;
        }

        void Logger::SetSpanId(const String& spanId) {
            // spanId 只描述当前调用片段，不替代 requestId。
            localContext.spanId = spanId;
        }

        void Logger::SetRequestId(const String& requestId) {
            // requestId 面向业务请求排障，生命周期由调用方控制。
            localContext.requestId = requestId;
        }

        struct LoggerContextScope::LoggerContextScopeImpl {
            ThreadContext m_savedContext; // 构造瞬间的线程局部上下文快照
            bool m_restored = false;      // 析构幂等保护标记
        };

        LoggerContextScope::LoggerContextScope()
            : m_impl(new LoggerContextScopeImpl{ CaptureLocalContext(), false }) {
            // 构造只保存快照，不立即修改当前线程上下文。
        }

        LoggerContextScope::LoggerContextScope(const String& key, const String& value)
            : LoggerContextScope() {
            // 带字段构造在保存旧上下文后追加临时字段，析构会恢复旧状态。
            SetContextField(key, value);
        }

        LoggerContextScope::~LoggerContextScope() {
            if (!m_impl) return;

            // 析构恢复构造时快照，保证异常离开作用域也不会泄漏上下文。
            if (!m_impl->m_restored) {
                RestoreLocalContext(m_impl->m_savedContext);
                m_impl->m_restored = true;
            }

            // PImpl 手动释放以保持公开头的旧式 ABI 形状。
            delete m_impl;
            m_impl = nullptr;
        }

        void LoggerContextScope::SetThreadName(const String& name) {
            // Scope 内变更复用 Logger 静态入口，析构统一恢复快照。
            Logger::SetThreadName(name);
        }

        void LoggerContextScope::SetModule(const String& module) {
            // 模块字段随作用域临时覆盖，适合一次业务流程内的日志。
            Logger::SetModule(module);
        }

        void LoggerContextScope::SetCategory(const String& category) {
            // 类别字段用于细分 Scope 内日志主题。
            Logger::SetCategory(category);
        }

        void LoggerContextScope::SetTraceId(const String& traceId) {
            // 调用链字段作用域化后可避免线程复用时串线。
            Logger::SetTraceId(traceId);
        }

        void LoggerContextScope::SetSpanId(const String& spanId) {
            // spanId 通常短于 traceId 生命周期，适合放入局部 Scope。
            Logger::SetSpanId(spanId);
        }

        void LoggerContextScope::SetRequestId(const String& requestId) {
            // requestId 与调用链字段互不覆盖，便于业务排障。
            Logger::SetRequestId(requestId);
        }

        void LoggerContextScope::SetContextField(const String& key, const String& value) {
            // 自定义字段使用同 key 覆盖语义，避免重复字段。
            Logger::SetContextField(key, value);
        }

        void LoggerContextScope::RemoveContextField(const String& key) {
            // 删除只影响当前 Scope 内后续消息，析构仍恢复旧字段集。
            Logger::RemoveContextField(key);
        }

        void LoggerContextScope::ClearContext() {
            // 清空业务上下文但保留 threadName，和 Logger::ClearContext 语义一致。
            Logger::ClearContext();
        }

        bool Logger::IsLevelEnabled(Level level) const {
            if (!m_impl) return false;

            Level minLevel = DecodeLevel(m_impl->m_minLevel.load(std::memory_order_acquire)); // 当前全局过滤级别
            // 这里是热路径，只做原子读取和整数比较，不触碰队列锁。
            return level >= minLevel;
        }

        bool Logger::Start() {
            if (!m_impl) return false;

            std::lock_guard<std::mutex> lock(m_impl->m_startMutex);
            // 若旧线程已收到 stop，先 join 干净再创建新线程。
            if (m_impl->m_worker.joinable()) {
                if (!m_impl->m_stop.load(std::memory_order_acquire)) return true;
                m_impl->m_worker.join();
            }
            if (m_impl->m_retryWorker.joinable()) {
                m_impl->m_retryWorker.join();
            }

            bool expected = true; // 只有停止状态才能切换到运行状态
            if (!m_impl->m_stop.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
                return true;
            }

            try {
                // 两个 worker 分离，普通分发不被失败重试退避阻塞。
                m_impl->m_worker = std::thread([this] { ProcessLoop(); });
                m_impl->m_retryWorker = std::thread([this] { RetryLoop(); });
            }
            catch (...) {
                // 任一线程创建失败都回到停止态，并等待已创建线程退出。
                m_impl->m_stop.store(true, std::memory_order_release);
                m_impl->m_cv.notify_all();
                m_impl->m_retryCv.notify_all();
                if (m_impl->m_worker.joinable()) m_impl->m_worker.join();
                throw;
            }

            return m_impl->m_worker.joinable() && m_impl->m_retryWorker.joinable();
        }

        bool Logger::Flush(std::chrono::milliseconds timeout) {
            if (!m_impl) return false;

            const bool unlimitedTimeout = timeout == std::chrono::milliseconds::max(); // max 表示无限等待 drain
            const auto deadline = unlimitedTimeout
                ? std::chrono::steady_clock::time_point::max()
                : std::chrono::steady_clock::now() + timeout;
            // 主队列和重试队列共享同一个截止时间，避免总等待时间翻倍。
            auto remainingTimeout = [&] {
                if (unlimitedTimeout) return std::chrono::milliseconds::max();
                const auto now = std::chrono::steady_clock::now();
                if (now >= deadline) return std::chrono::milliseconds(0);
                return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
            };

            {
                std::lock_guard<std::mutex> startLock(m_impl->m_startMutex);
                // 未启动且无 worker 可唤醒时只能检查静态状态，不能等待条件变量。
                if (m_impl->m_stop.load(std::memory_order_acquire) && !m_impl->m_worker.joinable()) {
                    std::lock_guard<std::mutex> queueLock(m_impl->m_queueMtx);
                    std::lock_guard<std::mutex> retryLock(m_impl->m_retryMtx);
                    if (m_impl->m_queue.empty() && m_impl->m_inFlight == 0 &&
                        m_impl->m_retryQueue.empty() && m_impl->m_retryInFlight == 0) {
                        return true;
                    }

                    m_impl->m_flushTimeouts.fetch_add(1, std::memory_order_relaxed);
                    return false;
                }
            }

            bool drained = false;
            {
                std::unique_lock<std::mutex> lock(m_impl->m_queueMtx);
                // 主队列 drain 要求队列为空且 worker 没有正在写 Sink 的消息。
                if (unlimitedTimeout) {
                    m_impl->m_drainCv.wait(lock, [this] {
                        return m_impl->m_queue.empty() && m_impl->m_inFlight == 0;
                    });
                    drained = true;
                }
                else {
                    drained = m_impl->m_drainCv.wait_for(lock, remainingTimeout(), [this] {
                        return m_impl->m_queue.empty() && m_impl->m_inFlight == 0;
                    });
                }
            }

            if (!drained) {
                m_impl->m_flushTimeouts.fetch_add(1, std::memory_order_relaxed);
                return false;
            }

            bool retryDrained = false;
            {
                std::unique_lock<std::mutex> lock(m_impl->m_retryMtx);
                // 重试队列 drain 独立等待，确保失败 Sink 的延迟任务也完成。
                if (unlimitedTimeout) {
                    m_impl->m_retryDrainCv.wait(lock, [this] {
                        return m_impl->m_retryQueue.empty() && m_impl->m_retryInFlight == 0;
                    });
                    retryDrained = true;
                }
                else {
                    retryDrained = m_impl->m_retryDrainCv.wait_for(lock, remainingTimeout(), [this] {
                        return m_impl->m_retryQueue.empty() && m_impl->m_retryInFlight == 0;
                    });
                }
            }

            if (!retryDrained) {
                m_impl->m_flushTimeouts.fetch_add(1, std::memory_order_relaxed);
                return false;
            }

            std::shared_lock<std::shared_mutex> sinkLock(m_impl->m_sinkMtx);
            for (auto& sink : m_impl->m_sinks) {
                // Flush 只调用当前有效 Sink，已被替换的旧快照由 worker 自己持有。
                if (!sink.enabled || !sink.sink) continue;

                try {
                    sink.sink->Flush();
                }
                catch (const std::exception& ex) {
                    m_impl->m_sinkWriteFailures.fetch_add(1, std::memory_order_relaxed);
                    std::cerr << "[Logger Error] Sink flush failed: " << ex.what() << std::endl;
                }
                catch (...) {
                    m_impl->m_sinkWriteFailures.fetch_add(1, std::memory_order_relaxed);
                    std::cerr << "[Logger Error] Sink flush failed: Unknown exception" << std::endl;
                }
            }

            return true;
        }

        void Logger::Shutdown(bool clearSink) {
            (void)Shutdown(std::chrono::milliseconds::max(), clearSink);
        }

        bool Logger::Shutdown(std::chrono::milliseconds timeout, bool clearSink) {
            if (!m_impl) return false;

            // stop 先发布，再唤醒所有可能等待队列、容量或重试延迟的线程。
            m_impl->m_stop.store(true, std::memory_order_release);
            m_impl->m_cv.notify_all();
            m_impl->m_capacityCv.notify_all();
            m_impl->m_retryCv.notify_all();

            bool drained = Flush(timeout); // 超时版本允许调用方决定是否继续等待 join
            if (!drained && timeout != std::chrono::milliseconds::max()) {
                return false;
            }

            {
                std::lock_guard<std::mutex> lock(m_impl->m_startMutex);
                // join 在 stop 发布后执行，避免析构释放 impl 时 worker 仍访问内部状态。
                if (m_impl->m_worker.joinable()) {
                    m_impl->m_worker.join();
                }
                if (m_impl->m_retryWorker.joinable()) {
                    m_impl->m_retryWorker.join();
                }
            }

            if (clearSink) {
                std::unique_lock<std::shared_mutex> lock(m_impl->m_sinkMtx);
                // 清理 Sink 只在 worker 退出后执行，不会破坏正在写入的快照。
                m_impl->m_sinks.clear();
            }

            return drained;
        }

        void Logger::LogMessageString(Level level, const String& msg,
            const char* file, int line, const char* func) {
            String ownedMsg(msg); // 兼容旧模板实例和外部对象文件，随后复用右值热路径
            LogMessageString(level, std::move(ownedMsg), file, line, func);
        }

        void Logger::LogMessageString(Level level, String&& msg,
            const char* file, int line, const char* func) {
            if (!m_impl) return;
            if (m_impl->m_stop.load(std::memory_order_acquire)) {
                // 停止态拒绝新日志，避免 Shutdown 后重新激活队列。
                m_impl->m_droppedMessages.fetch_add(1, std::memory_order_relaxed);
                return;
            }

            Level minLevel = DecodeLevel(m_impl->m_minLevel.load(std::memory_order_acquire)); // 当前全局过滤级别
            if (level < minLevel) return;

            try {
                Message message; // 入队前构造完整快照，后台线程不再访问调用方上下文
                message.level = level;
                message.msg = std::move(msg);
                message.file = (file != nullptr) ? String(file) : String();
                message.line = line;
                message.tid = std::this_thread::get_id();
                message.threadName = CurrentThreadNameFallback();
                message.timestamp = std::chrono::system_clock::now();
                message.func = (func != nullptr) ? String(func) : String();
                message.debug = m_impl->m_debug.load(std::memory_order_acquire);
                message.minLevel = minLevel;
                message.encoding = DecodeEncoding(m_impl->m_encoding.load(std::memory_order_acquire));
                message.module = localContext.module;
                message.category = localContext.category;
                message.traceId = localContext.traceId;
                message.spanId = localContext.spanId;
                message.requestId = localContext.requestId;
                message.processId = CurrentProcessId();
                message.contextFields = localContext.fields;

                bool accepted = false; // 仅成功进入队列后才递增 accepted 并唤醒 worker
                {
                    std::unique_lock<std::mutex> lock(m_impl->m_queueMtx);
                    if (m_impl->m_stop.load(std::memory_order_acquire)) {
                        // 处理 Start/Shutdown 竞争：拿到队列锁后再次确认运行状态。
                        m_impl->m_droppedMessages.fetch_add(1, std::memory_order_relaxed);
                        return;
                    }

                    const LoggerOptions options = m_impl->m_options; // 当前队列策略快照
                    // 输出格式和 loggerName 与队列配置同锁读取，成为消息的不可变快照。
                    message.outputFormat = options.outputFormat;
                    message.loggerName = m_impl->m_loggerName;
                    if (options.maxQueueSize > 0) {
                        if (options.overflowPolicy == QueueOverflowPolicy::Block) {
                            // Block 策略等待 worker 释放容量，stop 信号也会唤醒等待者。
                            const auto hasCapacity = [this, options] {
                                return m_impl->m_stop.load(std::memory_order_acquire) ||
                                    m_impl->m_queue.size() < options.maxQueueSize;
                            };

                            if (options.enqueueTimeout.count() <= 0) {
                                m_impl->m_capacityCv.wait(lock, hasCapacity);
                            }
                            else if (!m_impl->m_capacityCv.wait_for(lock, options.enqueueTimeout, hasCapacity)) {
                                m_impl->m_enqueueTimeouts.fetch_add(1, std::memory_order_relaxed);
                                // 等待容量超时按丢弃处理，避免调用线程无限阻塞。
                                m_impl->m_droppedMessages.fetch_add(1, std::memory_order_relaxed);
                                return;
                            }

                            if (m_impl->m_stop.load(std::memory_order_acquire)) {
                                // 等待期间可能进入 Shutdown，醒来后不再接受新消息。
                                m_impl->m_droppedMessages.fetch_add(1, std::memory_order_relaxed);
                                return;
                            }
                        }

                        if (m_impl->m_queue.size() >= options.maxQueueSize) {
                            if (options.overflowPolicy == QueueOverflowPolicy::DropNewest) {
                                // DropNewest 直接放弃当前消息，保留队列内旧消息顺序。
                                m_impl->m_droppedMessages.fetch_add(1, std::memory_order_relaxed);
                                return;
                            }

                            if (options.overflowPolicy == QueueOverflowPolicy::DropOldest && !m_impl->m_queue.empty()) {
                                // DropOldest 为当前消息腾出一个槽位，统计被裁剪的旧消息。
                                m_impl->m_queue.pop_front();
                                m_impl->m_droppedMessages.fetch_add(1, std::memory_order_relaxed);
                            }
                        }
                    }

                    // push 后立即记录水位，Flush 只关心队列和 in-flight 归零。
                    m_impl->m_queue.push_back(std::move(message));
                    m_impl->m_queueHighWatermark = (std::max)(m_impl->m_queueHighWatermark, m_impl->m_queue.size());
                    accepted = true;
                }

                if (accepted) {
                    // 通知放在锁外，减少 worker 被唤醒后立即竞争同一把锁的概率。
                    m_impl->m_acceptedMessages.fetch_add(1, std::memory_order_relaxed);
                    m_impl->m_cv.notify_one();
                }
            }
            catch (const std::exception& ex) {
                // 构造消息或入队过程失败时不向调用方抛出，日志系统保持旁路特性。
                m_impl->m_droppedMessages.fetch_add(1, std::memory_order_relaxed);
                std::cerr << "[Logger Error] Failed to log message: " << ex.what() << std::endl;
            }
            catch (...) {
                // 未知异常同样只计入 dropped，避免日志失败破坏业务流程。
                m_impl->m_droppedMessages.fetch_add(1, std::memory_order_relaxed);
                std::cerr << "[Logger Error] Failed to log message: Unknown exception" << std::endl;
            }
        }
    }
}
