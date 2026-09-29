#pragma once
#include <LikesProgram/Logging/system/LikesProgramLoggingExport.hpp>
#include <LikesProgram/Logging/LoggerConfig.hpp>
#include <LikesProgram/Logging/sinks/Sink.hpp>
#include <source_location>
#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace LikesProgram {
    namespace Log {
        namespace Detail {
            // 全局级别快照服务模板热路径；Logger::LogMessageString 仍会使用 PImpl 权威状态二次确认。
            extern LIKESPROGRAM_LOGGING_API std::atomic<int> LoggerMinLevelSnapshot;

            // 禁用级别仅需原子读取和整数比较，避免为常见过滤路径调用非内联 PImpl 成员函数。
            inline bool IsLevelEnabledFast(Level level) noexcept {
                const int minLevel = LoggerMinLevelSnapshot.load(std::memory_order_relaxed); // 当前全局最低日志级别快照
                return static_cast<int>(level) >= minLevel;
            }
        }

        // 全局日志调度器，负责级别过滤、异步队列、Sink 分发和失败重试。
        class LIKESPROGRAM_LOGGING_API Logger {
        public:
            // 获取全局唯一 Logger，默认不自动启动。
            static Logger& Instance();

            // 获取全局唯一 Logger，可选择首次访问或已关闭时自动启动。
            static Logger& Instance(bool autoStart);

            // 获取全局唯一 Logger，并指定是否输出调用点调试信息。
            static Logger& Instance(bool autoStart, bool debug);

            // 设置全局日志级别，低于该级别的日志会被过滤。
            void SetLevel(Level level);

            // 设置 Sink 输出编码，主要服务 Windows 控制台等迁移场景。
            void SetEncoding(String::Encoding encoding);

            // 设置异步队列容量、背压策略和入队等待时间。
            void Configure(const LoggerOptions& options);

            // 返回当前 Logger 配置快照。
            LoggerOptions Options() const;

            // 返回队列、丢弃、失败和 Sink 数量等运行时统计快照。
            LoggerStats Stats() const;

            // 返回配置、统计、级别和编码等自诊断快照。
            LoggerDiagnostics Diagnostics() const;

            // 以文本或 JSON Lines 固定字段格式导出自诊断快照。
            String ExportDiagnostics(LogOutputFormat format = LogOutputFormat::Text) const;

            // 原子应用开放式日志配置，校验失败时保留当前运行配置。
            Status ApplyConfig(const LoggerConfig& config);

            // 返回当前生效的开放式配置快照。
            Result<LoggerConfig> EffectiveConfig() const;

            // 添加一个日志输出目标，空指针会被忽略。
            void AddSink(std::shared_ptr<Sink> sink);

            // 原子替换当前 Sink 列表，空指针条目会被忽略。
            void SetSinks(const std::vector<std::shared_ptr<Sink>>& sinks);

            // 清空当前 Sink 列表，后续日志只计入队列统计。
            void ClearSinks();

            // 设置 Logger 逻辑名称，之后写入的日志都会携带该名称快照。
            void SetLoggerName(const String& name);

            // 设置当前线程的展示名，用于替代底层 thread::id 输出。
            static void SetThreadName(const String& name);

            // 清除当前线程的展示名，后续日志回退到 thread::id。
            static void ClearThreadName();

            // 设置当前线程的固定上下文字段，空 key 会被忽略。
            static void SetContextField(const String& key, const String& value);

            // 删除当前线程的一个上下文字段。
            static void RemoveContextField(const String& key);

            // 清空当前线程的全部上下文字段与常用 trace 字段。
            static void ClearContext();

            // 设置当前线程的模块字段。
            static void SetModule(const String& module);

            // 设置当前线程的类别字段。
            static void SetCategory(const String& category);

            // 设置当前线程的 trace id 字段。
            static void SetTraceId(const String& traceId);

            // 设置当前线程的 span id 字段。
            static void SetSpanId(const String& spanId);

            // 设置当前线程的 request id 字段。
            static void SetRequestId(const String& requestId);

            // 格式化并提交一条日志记录，调用点由 source_location 捕获。
            void Log(Level level, const std::source_location& loc, const String& message) {
                if (!Detail::IsLevelEnabledFast(level)) return;

                LogMessageString(level, message,
                    loc.file_name(), loc.line(), loc.function_name());
            }

            template <typename... Args>
                requires (sizeof...(Args) > 0)
            void Log(Level level, const std::source_location& loc, const String& format, Args&&... args) {
                if (!Detail::IsLevelEnabledFast(level)) return;

                LogMessageString(level, String::Format(format, std::forward<Args>(args)...),
                    loc.file_name(), loc.line(), loc.function_name());
            }

            // 可变 UTF-16 数组必须按调用时内容建立异步快照，不能按地址缓存。
            template <size_t N>
            void Log(Level level, const std::source_location& loc,
                char16_t(&message)[N]) {
                if (!Detail::IsLevelEnabledFast(level)) return;

                LogMessageString(level,
                    String(std::u16string_view(message, N > 0 ? N - 1 : 0)),
                    loc.file_name(), loc.line(), loc.function_name());
            }

            // const UTF-16 数组按地址和调用点共享不可变文本快照。
            template <size_t N>
            void Log(Level level, const std::source_location& loc,
                const char16_t(&message)[N]) {
                if (!Detail::IsLevelEnabledFast(level)) return;

                LogMessageLiteral(level,
                    std::u16string_view(message, N > 0 ? N - 1 : 0), message,
                    loc.file_name(), loc.line(), loc.function_name());
            }

            // UTF-16 字面量入口先过滤级别，再构造 String，避免禁用日志产生格式文本分配。
            template <typename... Args>
                requires (sizeof...(Args) > 0)
            void Log(Level level, const std::source_location& loc, const char16_t* format, Args&&... args) {
                if (!Detail::IsLevelEnabledFast(level)) return;

                std::u16string_view view(format ? format : u""); // 格式串视图，不拥有调用方字面量
                LogMessageString(level, String::Format(view, std::forward<Args>(args)...),
                    loc.file_name(), loc.line(), loc.function_name());
            }

            template <typename Pointer>
                requires (std::is_same_v<std::remove_cvref_t<Pointer>, const char16_t*> ||
                    std::is_same_v<std::remove_cvref_t<Pointer>, char16_t*>)
            void Log(Level level, const std::source_location& loc, Pointer&& message) {
                if (!Detail::IsLevelEnabledFast(level)) return;

                LogMessageString(level, String(std::u16string_view(message ? message : u"")),
                    loc.file_name(), loc.line(), loc.function_name());
            }

            // UTF-16 view 入口服务缓存格式串和外部缓冲，禁用级别不复制格式文本。
            void Log(Level level, const std::source_location& loc, std::u16string_view message) {
                if (!Detail::IsLevelEnabledFast(level)) return;

                LogMessageString(level, String(message),
                    loc.file_name(), loc.line(), loc.function_name());
            }

            template <typename... Args>
                requires (sizeof...(Args) > 0)
            void Log(Level level, const std::source_location& loc, std::u16string_view format, Args&&... args) {
                if (!Detail::IsLevelEnabledFast(level)) return;

                LogMessageString(level, String::Format(format, std::forward<Args>(args)...),
                    loc.file_name(), loc.line(), loc.function_name());
            }

            // 同步分发到当前 Sink 快照，不要求 Start、不进入队列，也不调度失败重试。
            // 消息构造或任一符合条件的 Sink 写入失败时返回 false。
            template <typename... Args>
            bool LogSync(Level level, const std::source_location& loc,
                const String& format, Args&&... args) {
                if (!Detail::IsLevelEnabledFast(level)) return true;

                try {
                    if constexpr (sizeof...(args) == 0) {
                        return LogMessageStringSync(level, format,
                            loc.file_name(), loc.line(), loc.function_name());
                    }
                    return LogMessageStringSync(level,
                        String::Format(format, std::forward<Args>(args)...),
                        loc.file_name(), loc.line(), loc.function_name());
                }
                catch (const std::exception& ex) {
                    return RecordSynchronousConstructionFailure(ex.what());
                }
                catch (...) {
                    return RecordSynchronousConstructionFailure(nullptr);
                }
            }

            // 可变 UTF-16 数组每次校验当前内容，覆盖同一地址原位更新。
            template <size_t N>
            bool LogSync(Level level, const std::source_location& loc,
                char16_t(&message)[N]) {
                if (!Detail::IsLevelEnabledFast(level)) return true;

                try {
                    return LogMessageViewSync(level,
                        std::u16string_view(message, N > 0 ? N - 1 : 0),
                        loc.file_name(), loc.line(), loc.function_name());
                }
                catch (const std::exception& ex) {
                    return RecordSynchronousConstructionFailure(ex.what());
                }
                catch (...) {
                    return RecordSynchronousConstructionFailure(nullptr);
                }
            }

            // const UTF-16 数组同步入口沿用相同过滤、格式化和失败统计语义。
            template <size_t N>
            bool LogSync(Level level, const std::source_location& loc,
                const char16_t(&message)[N]) {
                if (!Detail::IsLevelEnabledFast(level)) return true;

                try {
                    return LogMessageLiteralSync(level,
                        std::u16string_view(message, N > 0 ? N - 1 : 0), message,
                        loc.file_name(), loc.line(), loc.function_name());
                }
                catch (const std::exception& ex) {
                    return RecordSynchronousConstructionFailure(ex.what());
                }
                catch (...) {
                    return RecordSynchronousConstructionFailure(nullptr);
                }
            }

            // UTF-16 指针和带格式参数入口保留内容检查，不能假定外部缓冲不可变。
            template <typename... Args>
                requires (sizeof...(Args) > 0)
            bool LogSync(Level level, const std::source_location& loc,
                const char16_t* format, Args&&... args) {
                if (!Detail::IsLevelEnabledFast(level)) return true;

                try {
                    std::u16string_view view(format ? format : u""); // 不拥有调用方格式串
                    if constexpr (sizeof...(args) == 0) {
                        return LogMessageViewSync(level, view,
                            loc.file_name(), loc.line(), loc.function_name());
                    }
                    return LogMessageStringSync(level,
                        String::Format(view, std::forward<Args>(args)...),
                        loc.file_name(), loc.line(), loc.function_name());
                }
                catch (const std::exception& ex) {
                    return RecordSynchronousConstructionFailure(ex.what());
                }
                catch (...) {
                    return RecordSynchronousConstructionFailure(nullptr);
                }
            }

            // 普通 UTF-16 指针不具备字面量生命周期保证，按当前内容执行同步分发。
            template <typename Pointer>
                requires (std::is_same_v<std::remove_cvref_t<Pointer>, const char16_t*> ||
                    std::is_same_v<std::remove_cvref_t<Pointer>, char16_t*>)
            bool LogSync(Level level, const std::source_location& loc, Pointer&& message) {
                if (!Detail::IsLevelEnabledFast(level)) return true;

                try {
                    return LogMessageViewSync(level,
                        std::u16string_view(message ? message : u""),
                        loc.file_name(), loc.line(), loc.function_name());
                }
                catch (const std::exception& ex) {
                    return RecordSynchronousConstructionFailure(ex.what());
                }
                catch (...) {
                    return RecordSynchronousConstructionFailure(nullptr);
                }
            }

            // UTF-16 view 同步入口允许外部缓冲按值快照后直接分发。
            template <typename... Args>
            bool LogSync(Level level, const std::source_location& loc,
                std::u16string_view format, Args&&... args) {
                if (!Detail::IsLevelEnabledFast(level)) return true;

                try {
                    if constexpr (sizeof...(args) == 0) {
                        return LogMessageViewSync(level, format,
                            loc.file_name(), loc.line(), loc.function_name());
                    }
                    return LogMessageStringSync(level,
                        String::Format(format, std::forward<Args>(args)...),
                        loc.file_name(), loc.line(), loc.function_name());
                }
                catch (const std::exception& ex) {
                    return RecordSynchronousConstructionFailure(ex.what());
                }
                catch (...) {
                    return RecordSynchronousConstructionFailure(nullptr);
                }
            }

            // 启动日志后台线程，重复调用保持幂等。
            bool Start();

            // 等待队列 drain 并 Flush 所有 Sink，超时返回 false。
            bool Flush(std::chrono::milliseconds timeout);

            // 停止日志后台线程，可选择是否清空 Sink 列表。
            void Shutdown(bool clearSink = true);

            // 带超时的停止接口，返回是否在超时前完成 drain。
            bool Shutdown(std::chrono::milliseconds timeout, bool clearSink = true);

            ~Logger();

        private:
            // 单例构造只允许 Instance 调用。
            Logger(bool autoStart, bool debug);

            // 单例公共重载共用入口，避免无参 Instance 意外覆盖 debug 开关。
            static Logger& InstanceInternal(bool autoStart, bool debug, bool updateDebug);

            Logger(const Logger&) = delete;
            Logger& operator=(const Logger&) = delete;
            Logger(Logger&&) noexcept = delete;
            Logger& operator=(Logger&&) noexcept = delete;

            // 将已格式化文本送入队列，左值入口保留二进制兼容并复制成消息快照。
            void LogMessageString(Level level, const String& msg, const char* file, int line, const char* func);

            // 将临时格式化文本送入队列，右值入口直接移动进消息快照。
            void LogMessageString(Level level, String&& msg, const char* file, int line, const char* func);

            // UTF-16 数组入口以地址加内容命中缓存，异步队列共享其物化结果。
            void LogMessageLiteral(Level level, std::u16string_view msg, const void* literalKey,
                const char* file, int line, const char* func);

            // 动态文本和字面量共用紧凑异步快照入口。
            void LogMessageAsync(Level level, String* ownedMessage, std::u16string_view literal,
                const void* literalKey, const char* file, int line, const char* func);

            // 将左值消息复制后同步分发到当前 Sink 快照。
            bool LogMessageStringSync(Level level, const String& msg,
                const char* file, int line, const char* func);

            // 将临时消息直接同步分发到当前 Sink 快照。
            bool LogMessageStringSync(Level level, String&& msg,
                const char* file, int line, const char* func);

            // 无格式参数 UTF-16 入口直接复用 view，避免热路径先构造临时 String。
            bool LogMessageViewSync(Level level, std::u16string_view msg,
                const char* file, int line, const char* func);

            // UTF-16 数组以地址和内容命中缓存，避免栈地址复用返回旧消息。
            bool LogMessageLiteralSync(Level level, std::u16string_view msg, const void* literalKey,
                const char* file, int line, const char* func);

            // 三种同步文本入口共用缓存和分发实现；movable 非空时允许转移文本所有权。
            bool LogMessageSync(Level level, const String* msg, String* movable,
                std::u16string_view view, const void* literalKey, bool useView,
                const char* file, int line, const char* func);

            // 记录同步消息构造失败并转换为稳定的 false 返回值。
            bool RecordSynchronousConstructionFailure(const char* reason) noexcept;

            // 快速判断日志级别是否会通过过滤，避免禁用级别提前格式化参数。
            bool IsLevelEnabled(Level level) const;

            // 后台分发循环，负责从队列取消息并调用所有 Sink。
            void ProcessLoop();

            // 后台重试循环，负责处理 Sink 写失败后的延迟重试任务。
            void RetryLoop();

            struct LoggerImpl;
            LoggerImpl* m_impl = nullptr; // 唯一拥有的 Logger 实现对象
        };

        // 线程局部日志上下文作用域，析构时恢复进入作用域前的上下文字段。
        class LIKESPROGRAM_LOGGING_API LoggerContextScope {
        public:
            // 保存当前线程上下文，析构时自动恢复。
            LoggerContextScope();

            // 保存当前线程上下文，并临时设置一个自定义字段。
            LoggerContextScope(const String& key, const String& value);

            // 退出作用域时恢复构造时的线程上下文。
            ~LoggerContextScope();

            LoggerContextScope(const LoggerContextScope&) = delete;
            LoggerContextScope& operator=(const LoggerContextScope&) = delete;
            LoggerContextScope(LoggerContextScope&&) noexcept = delete;
            LoggerContextScope& operator=(LoggerContextScope&&) noexcept = delete;

            // 临时设置当前线程展示名。
            void SetThreadName(const String& name);

            // 临时设置当前线程模块字段。
            void SetModule(const String& module);

            // 临时设置当前线程类别字段。
            void SetCategory(const String& category);

            // 临时设置当前线程 trace id。
            void SetTraceId(const String& traceId);

            // 临时设置当前线程 span id。
            void SetSpanId(const String& spanId);

            // 临时设置当前线程 request id。
            void SetRequestId(const String& requestId);

            // 临时设置当前线程自定义上下文字段。
            void SetContextField(const String& key, const String& value);

            // 临时删除当前线程自定义上下文字段。
            void RemoveContextField(const String& key);

            // 临时清空当前线程上下文字段，不清除线程展示名。
            void ClearContext();

        private:
            struct LoggerContextScopeImpl;
            LoggerContextScopeImpl* m_impl = nullptr; // 保存构造时上下文快照
        };
    }

    // 提供与 Config::Configuration 一致的顶层主入口短别名。
    using Logger = Log::Logger;

#define LogTrace(msg, ...) (LikesProgram::Log::Logger::Instance().Log(LikesProgram::Log::Level::Trace, std::source_location::current(), msg, ##__VA_ARGS__))
#define LogDebug(msg, ...) (LikesProgram::Log::Logger::Instance().Log(LikesProgram::Log::Level::Debug, std::source_location::current(), msg, ##__VA_ARGS__))
#define LogInfo(msg, ...)  (LikesProgram::Log::Logger::Instance().Log(LikesProgram::Log::Level::Info,  std::source_location::current(), msg, ##__VA_ARGS__))
#define LogWarn(msg, ...)  (LikesProgram::Log::Logger::Instance().Log(LikesProgram::Log::Level::Warn,  std::source_location::current(), msg, ##__VA_ARGS__))
#define LogError(msg, ...) (LikesProgram::Log::Logger::Instance().Log(LikesProgram::Log::Level::Error, std::source_location::current(), msg, ##__VA_ARGS__))
#define LogFatal(msg, ...) (LikesProgram::Log::Logger::Instance().Log(LikesProgram::Log::Level::Fatal, std::source_location::current(), msg, ##__VA_ARGS__))
}
