#pragma once
#include <LikesProgram/Threading/system/LikesProgramThreadingExport.hpp>
#include <LikesProgram/Threading/ThreadPoolObserver.hpp>
#include <LikesProgram/Threading/ThreadPool.hpp>

namespace LikesProgram {
    namespace Threading {
        // 返回 Threading 包名，用于测试、示例和诊断输出。
        LIKESPROGRAM_THREADING_API const char* PackageName() noexcept;

        // 返回 Threading 包当前跟随的 LikesProgram 统一版本号。
        LIKESPROGRAM_THREADING_API const char* PackageVersion() noexcept;

        // 表示 Threading 包目标已被成功链接到当前进程。
        LIKESPROGRAM_THREADING_API bool PackageAvailable() noexcept;
    }
}
