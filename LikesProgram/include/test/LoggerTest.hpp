#pragma once
#include "../LikesProgram/Logger.hpp"
#include "../LikesProgram/String.hpp"

namespace LoggerTest {
	void Test() {
        // 初始化日志
        auto& logger = LikesProgram::Logger::Instance();
#ifdef _WIN32
        logger.SetEncoding(LikesProgram::String::Encoding::GBK);
#endif
#ifdef _DEBUG
        logger.SetLevel(LikesProgram::Logger::LogLevel::Debug);
#else
        logger.SetLevel(LogLevel::Info);
#endif
        logger.AddSink(LikesProgram::CreateConsoleSink());
        //logger.AddSink(CreateFileSink("app.log")); // 输出到文件
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
	}
}