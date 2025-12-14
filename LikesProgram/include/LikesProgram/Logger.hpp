#pragma once
#include "system/LikesProgramLibExport.hpp"
#include <string>
#include <chrono>
#include <memory>
#include "String.hpp"
#include <thread>
#include <source_location>

namespace LikesProgram {
    class LIKESPROGRAM_API Logger {
    public:
        enum class LogLevel {
            Trace = 0,  // 最详细的日志, 用来跟踪程序的细粒度行为
            Debug,      // 详细日志, 用来记录开发过程中有用的信息
            Info,       // 运行时信息, 代表程序的正常运行状态
            Warn,       // 警告信息, 代表程序可能发生的错误
            Error,      // 错误信息, 程序出现了问题，某些功能可能失效
            Fatal       // 致命错误信息, 代表程序无法继续运行
        };

        struct LogMessage {
            LogLevel level = LogLevel::Trace;
            String msg;
            String file;
            int line = 0;
            std::thread::id tid;
            String threadName;
            std::chrono::system_clock::time_point timestamp;
            String func;
            LogLevel minLevel;
            String::Encoding encoding = String::Encoding::UTF8;
            bool debug = true;
        };

        // ILogSink 抽象接口
        class ILogSink {
        public:
            virtual ~ILogSink() = default;

            // 写日志接口（由具体子类实现）
            virtual void Write(const LogMessage& message) = 0;
            // 对子类开放
        protected:
            // 辅助函数：格式化日志内容（给子类调用）
            String FormatLogMessage(const LogMessage& message);
            // 辅助函数：日志级别转字符串（给子类调用）
            const String LevelToString(LogLevel lvl);
        };

        // 获取全局唯一实例
        static Logger& Instance();
        static Logger& Instance(bool autoStart);
        static Logger& Instance(bool autoStart, bool debug);

        // 设置全局日志级别
        // 低于该级别的日志将被过滤掉
        void SetLevel(LogLevel level);

        // 设置日志输出编码
        void SetEncoding(String::Encoding encoding);

        // 添加一个日志输出目标（Sink）
        void AddSink(std::shared_ptr<ILogSink> sink);

        // 格式化面板
        template <typename... Args>
        void Log(LogLevel level, const std::source_location& loc, const String& format, Args&&... args) {
            if constexpr (sizeof...(args) == 0) {
                // 无参数，直接输出原始字符串
                LogMessageString(level, format, loc.file_name(), loc.line(), loc.function_name());
            }
            else {
                // 有参数，执行格式化
                LogMessageString(level,
                    String::Format(format, std::forward<Args>(args)...),
                    loc.file_name(), loc.line(), loc.function_name());
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
        void LogMessageString(LogLevel level, const String& msg,
            const char* file, int line, const char* func);

        // 日志处理循环（后台线程执行）
        void ProcessLoop();

        struct LoggerImpl;
        LoggerImpl* m_impl; // 指针方式隐藏具体实现，减少编译依赖
        
        // 内置工厂
    public:
        // 创建一个控制台日志输出
        static std::shared_ptr<Logger::ILogSink> CreateConsoleSink();
        // 创建一个文件日志输出
        static std::shared_ptr<Logger::ILogSink> CreateFileSink(const String& filename);
    };

    // 宏接口
#define LOG_TRACE(msg, ...) (LikesProgram::Logger::Instance().Log(LikesProgram::Logger::LogLevel::Trace, std::source_location::current(), msg, ##__VA_ARGS__))
#define LOG_DEBUG(msg, ...) (LikesProgram::Logger::Instance().Log(LikesProgram::Logger::LogLevel::Debug, std::source_location::current(), msg, ##__VA_ARGS__))
#define LOG_INFO(msg, ...)  (LikesProgram::Logger::Instance().Log(LikesProgram::Logger::LogLevel::Info,  std::source_location::current(), msg, ##__VA_ARGS__))
#define LOG_WARN(msg, ...)  (LikesProgram::Logger::Instance().Log(LikesProgram::Logger::LogLevel::Warn,  std::source_location::current(), msg, ##__VA_ARGS__))
#define LOG_ERROR(msg, ...) (LikesProgram::Logger::Instance().Log(LikesProgram::Logger::LogLevel::Error, std::source_location::current(), msg, ##__VA_ARGS__))
#define LOG_FATAL(msg, ...) (LikesProgram::Logger::Instance().Log(LikesProgram::Logger::LogLevel::Fatal, std::source_location::current(), msg, ##__VA_ARGS__))
}
