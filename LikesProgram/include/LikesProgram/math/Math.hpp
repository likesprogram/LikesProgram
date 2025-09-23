#pragma once
#include <atomic>
#include <limits>

namespace LikesProgram {
	namespace Math {
		// 常量
		constexpr double PI = 3.1415926535897932384626433832795;
		constexpr double EPSILON = 1e-9; // 浮点比较精度

		// 最大值更新
		template <typename T1, typename T2>
		inline void UpdateMax(std::atomic<T1>& target, T2 value) {
			static_assert(std::is_integral_v<T1> || std::is_floating_point_v<T1> ||
				std::is_integral_v<T2> || std::is_floating_point_v<T2>,
				"UpdateMax requires integral or floating type");
			T1 old = target.load(std::memory_order_relaxed);
			while (value > old && !target.compare_exchange_weak(old, value, std::memory_order_release, std::memory_order_relaxed)) {}
		}

		// 更新最小值
		template <typename T1, typename T2>
		inline void UpdateMin(std::atomic<T1>& target, T2 value) {
			static_assert(std::is_integral_v<T1> || std::is_floating_point_v<T1> ||
				std::is_integral_v<T2> || std::is_floating_point_v<T2>,
				"UpdateMax requires integral or floating type");
			T1 old = target.load(std::memory_order_relaxed);
			while (value < old && !target.compare_exchange_weak(old, value, std::memory_order_release, std::memory_order_relaxed)) {}
		}

		template <typename T>
		inline double Average(T sum, size_t count) {
			static_assert(std::is_integral_v<T> || std::is_floating_point_v<T>,
				"EMA requires integral or floating type");
			return count == 0 ? 0.0 : sum / static_cast<double>(count);
		}

		// EMA (指数移动平均数)
		template <typename T1, typename T2>
		inline double EMA(T1 previous, T2 value, double alpha = 0.9) {
			static_assert(std::is_integral_v<T1> || std::is_floating_point_v<T1> ||
				std::is_integral_v<T2> || std::is_floating_point_v<T2>,
				"EMA requires integral or floating type");
            return static_cast<T1>(previous * alpha + static_cast<T1>(value) * (1.0 - alpha));
		}

		// 时间转换，纳秒/毫秒/秒
		inline long long NsToMs(long long ns) {
			return ns / 1'000'000;
		}
        inline long long NsToS(long long ns) {
			return ns / 1'000'000'000;
		}
        inline long long MsToNs(long long ms) {
			return ms * 1'000'000;
		}
        inline long long MsToS(long long ms) {
			return ms / 1'000;
		}
        inline long long SToNs(long long s) {
			return s * 1'000'000'000;
		}
        inline long long SToMs(long long s) {
			return s * 1'000;
		}

    }
}

