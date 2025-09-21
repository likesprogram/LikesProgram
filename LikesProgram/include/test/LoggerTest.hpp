#pragma once
#include "../LikesProgram/Logger.hpp"
#include "../LikesProgram/String.hpp"

namespace LoggerTest {
    // 自定义网络输出 Sink
    class NetworkSink : public LikesProgram::Logger::ILogSink {
    public:
        NetworkSink(const std::string& serverAddress) : server(serverAddress) {}

        void Write(const LikesProgram::Logger::LogMessage& message,
            LikesProgram::Logger::LogLevel minLevel,
            LikesProgram::String::Encoding encoding) override {
            LikesProgram::String formatted = FormatLogMessage(message, minLevel);
            SendToServer(formatted.ToStdString(encoding));
        }

    private:
        std::string server;

        void SendToServer(const std::string& payload) {
            // 这里可以实现 HTTP/TCP 发送逻辑
            // 示例中仅打印到控制台表示发送
            std::cout << "[SEND TO SERVER " << server << "] " << payload << std::endl;
        }
    };

    // 工厂函数
    std::shared_ptr<LikesProgram::Logger::ILogSink> CreateNetworkSink(const std::string& serverAddress) {
        return std::make_shared<NetworkSink>(serverAddress);
    }

	void Test() {
        // 初始化日志
        auto& logger = LikesProgram::Logger::Instance();
#ifdef _WIN32
        logger.SetEncoding(LikesProgram::String::Encoding::GBK);
#endif
#ifdef _DEBUG
        logger.SetLevel(LikesProgram::Logger::LogLevel::Debug);
#else
        logger.SetLevel(LikesProgram::Logger::LogLevel::Info);
#endif
        // 内置控制台输出 Sink
        logger.AddSink(LikesProgram::CreateConsoleSink()); // 输出到控制台
        // 内置输出到文件 Sink
        logger.AddSink(LikesProgram::CreateFileSink(u"app.log")); // 输出到文件
        // 自定义网络输出 Sink
        logger.AddSink(CreateNetworkSink("127.0.0.1:9000")); // 自定义输出 Sink
#ifdef _DEBUG
        LOG_TRACE(u"trace message 日志输出");   // 不会输出
        LOG_DEBUG(u"debug message 日志输出");   // 会输出
        LOG_INFO(u"info message 日志输出");     // 会输出
        LOG_WARN(u"warn message 日志输出");     // 会输出
        LOG_ERROR(u"error message 日志输出");   // 会输出
        LOG_FATAL(u"fatal message 日志输出");   // 会输出
#else
        LOG_TRACE(u"trace message 日志输出");   // 不会输出
        LOG_DEBUG(u"debug message 日志输出");   // 不会输出
        LOG_INFO(u"info message 日志输出");     // 会输出
        LOG_WARN(u"warn message 日志输出");     // 会输出
        LOG_ERROR(u"error message 日志输出");   // 会输出
        LOG_FATAL(u"fatal message 日志输出");   // 会输出
#endif

        // 设置线程名
        LikesProgram::CoreUtils::SetCurrentThreadName(u"主线程");
#ifdef _DEBUG
        LOG_TRACE(u"trace message 日志输出");   // 不会输出
        LOG_DEBUG(u"debug message 日志输出");   // 会输出
        LOG_INFO(u"info message 日志输出");     // 会输出
        LOG_WARN(u"warn message 日志输出");     // 会输出
        LOG_ERROR(u"error message 日志输出");   // 会输出
        LOG_FATAL(u"fatal message 日志输出");   // 会输出
#else
        LOG_TRACE(u"trace message 日志输出");   // 不会输出
        LOG_DEBUG(u"debug message 日志输出");   // 不会输出
        LOG_INFO(u"info message 日志输出");     // 会输出
        LOG_WARN(u"warn message 日志输出");     // 会输出
        LOG_ERROR(u"error message 日志输出");   // 会输出
        LOG_FATAL(u"fatal message 日志输出");   // 会输出
#endif
        std::this_thread::sleep_for(std::chrono::seconds(1)); // 给后台线程一点时间输出
        logger.Shutdown();
	}
}