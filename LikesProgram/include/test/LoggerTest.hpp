#pragma once
#include "../LikesProgram/log/Logger.hpp"
#include "../LikesProgram/log/sinks/ConsoleSink.hpp"
#include "../LikesProgram/log/sinks/FileSink.hpp"
#include "../LikesProgram/String.hpp"

namespace LoggerTest {
    // 自定义网络输出 Sink
    class NetworkSink : public LikesProgram::Log::Sink {
    public:
        NetworkSink(const std::string& serverAddress) : server(serverAddress) {}

        void Write(const LikesProgram::Log::Message& message) override {
            LikesProgram::String formatted = FormatLogMessage(message);
            SendToServer(formatted.ToStdString(message.encoding));
        }

        // 工厂函数
        static std::shared_ptr<LikesProgram::Log::Sink> CreateSink(const std::string& serverAddress) {
            return std::make_shared<NetworkSink>(serverAddress);
        }
    private:
        std::string server;

        void SendToServer(const std::string& payload) {
            // 这里可以实现 HTTP/TCP 发送逻辑
            // 示例中仅打印到控制台表示发送
            std::cout << "[SEND TO SERVER " << server << "] " << payload << std::endl;
        }
    };

	void Test() {
        // 初始化日志
#ifdef _DEBUG
        auto& logger = LikesProgram::Log::Logger::Instance(true, true);
        logger.SetLevel(LikesProgram::Log::Level::Debug);
#else
        auto& logger = LikesProgram::Log::Logger::Instance(true);
        logger.SetLevel(LikesProgram::Log::Level::Info);
#endif

#ifdef _WIN32
        logger.SetEncoding(LikesProgram::String::Encoding::GBK);
#endif
        // 内置控制台输出 Sink
        logger.AddSink(LikesProgram::Log::ConsoleSink::CreateSink()); // 输出到控制台
        // 内置输出到文件 Sink
        logger.AddSink(LikesProgram::Log::FileSink::CreateSink(
            u"./logs",      // 输出目录
            u"Logger.log",  // 文件名称
            30              // 单文件 最大容量 (MB)
        ));
        // 自定义网络输出 Sink
        logger.AddSink(NetworkSink::CreateSink("127.0.0.1:9000")); // 自定义输出 Sink
#ifdef _DEBUG
        LogTrace(u"trace message 日志输出 {}");   // 不会输出
        LogDebug(u"debug message 日志输出");   // 会输出
        LogInfo(u"info message 日志输出");     // 会输出
        LogWarn(u"warn message 日志输出");     // 会输出
        LogError(u"error message 日志输出");   // 会输出
        LogFatal(u"fatal message 日志输出");   // 会输出
#else
        LogTrace(u"trace message 日志输出");   // 不会输出
        LogDebug(u"debug message 日志输出");   // 不会输出
        LogInfo(u"info message 日志输出");     // 会输出
        LogWarn(u"warn message 日志输出");     // 会输出
        LogError(u"error message 日志输出");   // 会输出
        LogFatal(u"fatal message 日志输出");   // 会输出
#endif

        // 格式化输出
        LogTrace(u"trace message 格式化输出 LogLevel：{}", (int)LikesProgram::Log::Level::Trace);   // 不会输出
        LogDebug(u"debug message 格式化输出 LogLevel：{}", (int)LikesProgram::Log::Level::Debug);   // 会输出
        LogInfo(u"info message 格式化输出 LogLevel：{}", (int)LikesProgram::Log::Level::Info);    // 会输出
        LogWarn(u"warn message 格式化输出 LogLevel：{}", (int)LikesProgram::Log::Level::Warn);    // 会输出
        LogError(u"error message 格式化输出 LogLevel：{}", (int)LikesProgram::Log::Level::Error);   // 会输出
        LogFatal(u"fatal message 格式化输出 LogLevel：{}", (int)LikesProgram::Log::Level::Fatal);   // 会输出
        logger.Shutdown();
	}
}