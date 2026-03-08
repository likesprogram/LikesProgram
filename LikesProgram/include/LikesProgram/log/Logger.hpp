#pragma once
#include "../system/LikesProgramLibExport.hpp"
#include "sinks/Sink.hpp"
#include <source_location>

namespace LikesProgram {
    namespace Log {
        class LIKESPROGRAM_API Logger {
        public:
            // 获取全局唯一实例
            static Logger& Instance();
            static Logger& Instance(bool autoStart);
            static Logger& Instance(bool autoStart, bool debug);

            // 设置全局日志级别
            // 低于该级别的日志将被过滤掉
            void SetLevel(Level level);

            // 设置日志输出编码
            void SetEncoding(String::Encoding encoding);

            // 添加一个日志输出目标（Sink）
            void AddSink(std::shared_ptr<Sink> sink);

            // 格式化面板
            template <typename... Args>
            void Log(Level level, const std::source_location& loc, const String& format, Args&&... args) {
                if constexpr (sizeof...(args) == 0) {
                    // 无参数，直接输出原始字符串
                    LogMessageString(level, format, loc.file_name(), loc.line(), loc.function_name());
                }
                else {
                    // 有参数，执行格式化
                    LogMessageString(level, String::Format(format, std::forward<Args>(args)...), loc.file_name(), loc.line(), loc.function_name());
                }
            }

            // 启动日志系统（创建后台线程）
            bool Start();

            // 停止日志系统（结束后台线程，清理资源）
            void Shutdown(bool clearSink = true);

            ~Logger();
        private:
            // 单例
            Logger(bool autoStart, bool debug);

            // 禁止拷贝和赋值
            Logger(const Logger&) = delete;
            Logger& operator=(const Logger&) = delete;

            // 禁止移动和赋值（防止用户误操作破坏单例）
            Logger(Logger&&) noexcept = delete;
            Logger& operator=(Logger&&) noexcept = delete;

            // 记录一条日志
            void LogMessageString(Level level, const String& msg, const char* file, int line, const char* func);

            // 日志处理循环（后台线程执行）
            void ProcessLoop();

            struct LoggerImpl;
            LoggerImpl* m_impl; // 指针方式隐藏具体实现，减少编译依赖
        };
    }

// 宏接口
#define LogTrace(msg, ...) (LikesProgram::Log::Logger::Instance().Log(LikesProgram::Log::Level::Trace, std::source_location::current(), msg, ##__VA_ARGS__))
#define LogDebug(msg, ...) (LikesProgram::Log::Logger::Instance().Log(LikesProgram::Log::Level::Debug, std::source_location::current(), msg, ##__VA_ARGS__))
#define LogInfo(msg, ...)  (LikesProgram::Log::Logger::Instance().Log(LikesProgram::Log::Level::Info,  std::source_location::current(), msg, ##__VA_ARGS__))
#define LogWarn(msg, ...)  (LikesProgram::Log::Logger::Instance().Log(LikesProgram::Log::Level::Warn,  std::source_location::current(), msg, ##__VA_ARGS__))
#define LogError(msg, ...) (LikesProgram::Log::Logger::Instance().Log(LikesProgram::Log::Level::Error, std::source_location::current(), msg, ##__VA_ARGS__))
#define LogFatal(msg, ...) (LikesProgram::Log::Logger::Instance().Log(LikesProgram::Log::Level::Fatal, std::source_location::current(), msg, ##__VA_ARGS__))
}
