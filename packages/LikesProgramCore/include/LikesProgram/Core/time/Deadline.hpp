#pragma once
#include <LikesProgram/Core/time/Clock.hpp>

namespace LikesProgram {
    namespace Time {
        // 单调时钟截止时间，供上层组件统一表达超时。
        class Deadline {
        public:
            // 构造无限截止时间。
            Deadline() = default;

            // 从绝对单调时间点构造截止时间。
            static Deadline At(SteadyTimePoint timePoint) noexcept {
                Deadline deadline; // 输出截止时间对象
                deadline.m_timePoint = timePoint;
                deadline.m_hasDeadline = true;
                return deadline;
            }

            // 从相对超时时长构造截止时间。
            static Deadline FromNow(Duration timeout) noexcept {
                return At(Clock::Now() + timeout);
            }

            // 构造无限截止时间。
            static Deadline Infinite() noexcept {
                return Deadline();
            }

            // 是否存在实际截止时间。
            bool HasDeadline() const noexcept {
                return m_hasDeadline;
            }

            // 判断当前是否已经超时。
            bool Expired() const noexcept {
                return m_hasDeadline && Clock::Now() >= m_timePoint;
            }

            // 返回剩余时间；无限截止时间返回 Duration::max()。
            Duration Remaining() const noexcept {
                if (!m_hasDeadline) return Duration::max();
                auto now = Clock::Now(); // 当前单调时间点
                if (now >= m_timePoint) return Duration::zero();
                return std::chrono::duration_cast<Duration>(m_timePoint - now);
            }

            // 返回内部单调时间点；无限截止时间返回默认时间点。
            SteadyTimePoint TimePoint() const noexcept {
                return m_timePoint;
            }

        private:
            SteadyTimePoint m_timePoint{}; // 截止时间点，仅在 m_hasDeadline 为 true 时有效
            bool m_hasDeadline = false;    // false 表示无限等待
        };
    }
}
