#pragma once
#include <LikesProgram/Logging/system/LikesProgramLoggingExport.hpp>
#include <LikesProgram/Logging/LoggerType.hpp>
#include <LikesProgram/Logging/LoggerConfig.hpp>
#include <LikesProgram/Logging/Logger.hpp>
#include <LikesProgram/Logging/sinks/Sink.hpp>
#include <LikesProgram/Logging/sinks/ConsoleSink.hpp>
#include <LikesProgram/Logging/sinks/FileSink.hpp>

namespace LikesProgram {
    namespace Logging {
        // 返回 Logging 包名，用于测试、示例和诊断输出。
        LIKESPROGRAM_LOGGING_API const char* PackageName() noexcept;

        // 返回 Logging 包当前跟随的 LikesProgram 统一版本号。
        LIKESPROGRAM_LOGGING_API const char* PackageVersion() noexcept;

        // 表示 Logging 包目标已被成功链接到当前进程。
        LIKESPROGRAM_LOGGING_API bool PackageAvailable() noexcept;
    }
}
