#pragma once
#include <LikesProgram/Core/time/Time.hpp>

namespace LikesProgram {
    namespace Time {
        // 单调时钟时间点，用于超时和耗时计算。
        using SteadyTimePoint = std::chrono::steady_clock::time_point;

        // 统一时钟入口，避免扩展包各自选择不同 clock。
        class Clock {
        public:
            // 返回单调时钟当前时间点。
            static SteadyTimePoint Now() noexcept {
                return std::chrono::steady_clock::now();
            }

            // 返回系统时钟当前时间点。
            static TimePoint SystemNow() noexcept {
                return std::chrono::system_clock::now();
            }

            // 返回单调时钟 epoch 至今的纳秒数。
            static Nanoseconds NowNs() noexcept {
                return std::chrono::duration_cast<Nanoseconds>(Now().time_since_epoch());
            }

            // 计算 begin 到当前的纳秒耗时。
            static Nanoseconds Since(SteadyTimePoint begin) noexcept {
                return std::chrono::duration_cast<Nanoseconds>(Now() - begin);
            }
        };
    }
}
