#pragma once
#include <LikesProgram/Metrics/system/LikesProgramMetricsExport.hpp>
#include <LikesProgram/Metrics/Metric.hpp>
#include <LikesProgram/Metrics/Counter.hpp>
#include <LikesProgram/Metrics/Gauge.hpp>
#include <LikesProgram/Metrics/Histogram.hpp>
#include <LikesProgram/Metrics/Summary.hpp>
#include <LikesProgram/Metrics/Registry.hpp>

namespace LikesProgram {
    namespace Metrics {
        // 返回 Metrics 包名，用于测试、示例和诊断输出。
        LIKESPROGRAM_METRICS_API const char* PackageName() noexcept;

        // 返回 Metrics 包当前跟随的 LikesProgram 统一版本号。
        LIKESPROGRAM_METRICS_API const char* PackageVersion() noexcept;

        // 表示 Metrics 包目标已被成功链接到当前进程。
        LIKESPROGRAM_METRICS_API bool PackageAvailable() noexcept;
    }
}
