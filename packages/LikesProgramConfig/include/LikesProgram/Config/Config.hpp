#pragma once
#include <LikesProgram/Config/system/LikesProgramConfigExport.hpp>
#include <LikesProgram/Config/ConfigValue.hpp>
#include <LikesProgram/Config/ConfigSchema.hpp>
#include <LikesProgram/Config/Configuration.hpp>

namespace LikesProgram {
    namespace Config {
        // 返回 Config 包名，用于测试、示例和诊断输出。
        LIKESPROGRAM_CONFIG_API const char* PackageName() noexcept;

        // 返回 Config 包当前跟随的 LikesProgram 统一版本号。
        LIKESPROGRAM_CONFIG_API const char* PackageVersion() noexcept;

        // 表示 Config 包目标已被成功链接到当前进程。
        LIKESPROGRAM_CONFIG_API bool PackageAvailable() noexcept;
    }
}
