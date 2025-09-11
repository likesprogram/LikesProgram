#pragma once
#include <atomic>
#include <limits>

namespace LikesProgram {
	namespace Math {
		// ����
		static constexpr double PI = 3.1415926535897932384626433832795;
		static constexpr double EPSILON = 1e-9; // ����ȽϾ���
		static constexpr double INF = std::numeric_limits<double>::infinity(); // �����

		// ���ֵ����
		template <typename T1, typename T2>
		static inline void UpdateMax(std::atomic<T1>& target, T2 value) {
			static_assert(std::is_integral_v<T1> || std::is_floating_point_v<T1> ||
				std::is_integral_v<T2> || std::is_floating_point_v<T2>,
				"UpdateMax requires integral or floating type");
			T1 old = target.load(std::memory_order_relaxed);
			while (value > old && !target.compare_exchange_weak(old, value, std::memory_order_release, std::memory_order_relaxed)) {}
		}

		// EMA (ָ���ƶ�ƽ����)
		template <typename T1, typename T2>
		static inline double EMA(T1 previous, T2 value, double alpha = 0.9) {
			static_assert(std::is_integral_v<T1> || std::is_floating_point_v<T1> ||
				std::is_integral_v<T2> || std::is_floating_point_v<T2>,
				"EMA requires integral or floating type");
            return static_cast<T1>(previous * alpha + static_cast<T1>(value) * (1.0 - alpha));
		}

		// ʱ��ת��������/����/��
		static inline long long NsToMs(long long ns) {
			return ns / 1'000'000;
		}
        static inline long long NsToS(long long ns) {
			return ns / 1'000'000'000;
		}
        static inline long long MsToNs(long long ms) {
			return ms * 1'000'000;
		}
        static inline long long MsToS(long long ms) {
			return ms / 1'000;
		}
        static inline long long SToNs(long long s) {
			return s * 1'000'000'000;
		}
        static inline long long SToMs(long long s) {
			return s * 1'000;
		}

    }
}

