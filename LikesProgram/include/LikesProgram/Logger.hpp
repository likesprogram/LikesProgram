#pragma once
#include "LikesProgramLibExport.hpp"
#include <string>
#include <memory>
#include "String.hpp"
#include <thread>

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
            String func;
            std::thread::id tid;
            String threadName;
            std::chrono::system_clock::time_point timestamp;
        };

        // ILogSink 抽象接口
        class ILogSink {
        public:
            virtual ~ILogSink() = default;

            // 写日志接口（由具体子类实现）
            virtual void Write(const LogMessage& message, LogLevel minLevel, String::Encoding encoding) = 0;
            // 对子类开放
        protected:
            // 辅助函数：格式化日志内容（给子类调用）
            String FormatLogMessage(const LogMessage& message, LogLevel minLevel);
            // 辅助函数：日志级别转字符串（给子类调用）
            const String LevelToString(LogLevel lvl);
        };

        // 获取全局唯一实例
        static Logger& Instance();

        // 设置全局日志级别
        // 低于该级别的日志将被过滤掉
        void SetLevel(LogLevel level);

        // 设置日志输出编码
        void SetEncoding(String::Encoding encoding);

        // 添加一个日志输出目标（Sink）
        void AddSink(std::shared_ptr<ILogSink> sink);

        // 记录一条日志（供宏 FW_LOG_xxx 使用）
        void Log(LogLevel level, const String& msg,
            const char* file, int line, const char* func);

        // 停止日志系统（结束后台线程，清理资源）
        void Shutdown();

    private:
        // 构造 / 析构（私有化，保证单例）
        Logger();
        ~Logger();

        // 禁止拷贝和赋值
        Logger(const Logger&) = delete;
        Logger& operator=(const Logger&) = delete;

        // 日志处理循环（后台线程执行）
        void ProcessLoop();

        // 内部实现细节（PImpl 隐藏）
        struct Impl;
        Impl* pImpl; // 指针方式隐藏具体实现，减少编译依赖
    };

    // 工厂函数
    std::shared_ptr<Logger::ILogSink> LIKESPROGRAM_API CreateConsoleSink();
    std::shared_ptr<Logger::ILogSink> LIKESPROGRAM_API CreateFileSink(const String& filename);

    // 宏接口
#define LOG_TRACE(msg) LikesProgram::Logger::Instance().Log(LikesProgram::Logger::LogLevel::Trace, msg, __FILE__, __LINE__, __func__)
#define LOG_DEBUG(msg) LikesProgram::Logger::Instance().Log(LikesProgram::Logger::LogLevel::Debug, msg, __FILE__, __LINE__, __func__)
#define LOG_INFO(msg)  LikesProgram::Logger::Instance().Log(LikesProgram::Logger::LogLevel::Info,  msg, __FILE__, __LINE__, __func__)
#define LOG_WARN(msg)  LikesProgram::Logger::Instance().Log(LikesProgram::Logger::LogLevel::Warn,  msg, __FILE__, __LINE__, __func__)
#define LOG_ERROR(msg) LikesProgram::Logger::Instance().Log(LikesProgram::Logger::LogLevel::Error, msg, __FILE__, __LINE__, __func__)
#define LOG_FATAL(msg) LikesProgram::Logger::Instance().Log(LikesProgram::Logger::LogLevel::Fatal, msg, __FILE__, __LINE__, __func__)
}
