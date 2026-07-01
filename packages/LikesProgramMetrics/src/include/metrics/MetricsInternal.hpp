#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>

namespace LikesProgram {
    namespace Metrics {
        namespace Internal {
            // 将非有限浮点值收敛为可导出的有限边界，避免 JSON/Prometheus 输出 inf/nan。
            inline double FiniteForExport(double value) {
                if (std::isfinite(value)) return value;
                return value < 0.0
                    ? std::numeric_limits<double>::lowest()
                    : std::numeric_limits<double>::max();
            }

            // 计算饱和浮点加法结果，确保长期运行累计值仍保持有限。
            inline double SaturatingAdd(double current, double delta) {
                if (!std::isfinite(current)) current = 0.0;
                if (!std::isfinite(delta)) return current;

                constexpr double maxValue = std::numeric_limits<double>::max(); // 正向导出上界
                constexpr double minValue = std::numeric_limits<double>::lowest(); // 负向导出下界
                if (delta > 0.0 && current > maxValue - delta) return maxValue;
                if (delta < 0.0 && current < minValue - delta) return minValue;

                const double next = current + delta; // 正常热路径只做一次浮点加法
                return std::isfinite(next) ? next : (delta >= 0.0 ? maxValue : minValue);
            }

            // 对原子 double 执行有限值饱和累加，避免 fetch_add 直接溢出为 inf。
            inline void AddFiniteSaturating(std::atomic<double>& target, double delta) {
                if (!std::isfinite(delta)) return;

                double current = target.load(std::memory_order_relaxed); // CAS 循环的当前快照
                while (true) {
                    const double next = SaturatingAdd(current, delta); // 候选有限累加结果
                    if (target.compare_exchange_weak(current, next,
                        std::memory_order_relaxed, std::memory_order_relaxed)) {
                        return;
                    }
                }
            }

            // 对 int64 计数做饱和加法，长期运行到上界后保持稳定。
            inline int64_t AddInt64Saturating(int64_t current, int64_t delta) {
                if (delta > 0 && current > std::numeric_limits<int64_t>::max() - delta) {
                    return std::numeric_limits<int64_t>::max();
                }
                if (delta < 0 && current < std::numeric_limits<int64_t>::min() - delta) {
                    return std::numeric_limits<int64_t>::min();
                }
                return current + delta;
            }

            // 对原子计数执行 +1 饱和更新，避免服务长期运行后有符号溢出。
            inline void IncrementSaturating(std::atomic<int64_t>& target) {
                int64_t current = target.load(std::memory_order_relaxed); // CAS 循环的计数快照
                while (current < std::numeric_limits<int64_t>::max()) {
                    const int64_t next = current + 1; // 单次样本计数递增
                    if (target.compare_exchange_weak(current, next,
                        std::memory_order_relaxed, std::memory_order_relaxed)) {
                        return;
                    }
                }
            }
        }
    }
}
