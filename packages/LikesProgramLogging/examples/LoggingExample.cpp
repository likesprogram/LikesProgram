#include <LikesProgram/Logging/Logging.hpp>
#include <iostream>

// Logging 示例展示包身份、Logger 启停和控制台 Sink 基础用法。
int main() {
    const char* packageName = LikesProgram::Logging::PackageName(); // 示例输出的组件名
    const char* packageVersion = LikesProgram::Logging::PackageVersion(); // 示例输出的组件版本

    std::cout << packageName << " " << packageVersion << " ready" << std::endl;

    auto& logger = LikesProgram::Log::Logger::Instance(true, true);
    logger.SetLevel(LikesProgram::Log::Level::Debug);
    logger.SetLoggerName(u"example");
    LikesProgram::Log::Logger::SetThreadName(u"main");
    LikesProgram::Log::Logger::SetModule(u"demo");
    LikesProgram::Log::Logger::SetTraceId(u"trace-example");
    logger.AddSink(LikesProgram::Log::ConsoleSink::CreateSink());
    logger.Log(LikesProgram::Log::Level::Info, std::source_location::current(),
        u"{} example started", u"Logging");
    logger.Shutdown();

    return LikesProgram::Logging::PackageAvailable() ? 0 : 1;
}
