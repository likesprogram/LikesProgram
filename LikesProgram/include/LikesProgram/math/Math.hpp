#pragma once
#include "Vector.hpp"
#include "Vector3.hpp"
#include "Vector4.hpp"
#include "PercentileSketch.hpp"
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

        // Vector2 -> Vector3，默认 z=0
        inline Vector3 ToVector3(const Vector& v, double z = 0.0) {
            return Vector3(v[0], v[1], z);
        }

        // Vector2 -> Vector4，默认 z=0, w=0
        inline Vector4 ToVector4(const Vector& v, double z = 0.0, double w = 0.0) {
            return Vector4(v[0], v[1], z, w);
        }

        // Vector3 -> Vector2，丢弃 z
        inline Vector ToVector2(const Vector3& v) {
            return Vector(v[0], v[1]);
        }

        // Vector3 -> Vector4，默认 w=0
        inline Vector4 ToVector4(const Vector3& v, double w = 0.0) {
            return Vector4(v[0], v[1], v[2], w);
        }

        // Vector4 -> Vector3，丢弃 w
        inline Vector3 ToVector3(const Vector4& v) {
            return Vector3(v[0], v[1], v[2]);
        }

        // Vector4 -> Vector2，丢弃 z,w
        inline Vector ToVector2(const Vector4& v) {
            return Vector(v[0], v[1]);
        }

        // Vector4 -> Vector3，使用 YZW 分量
        inline Vector3 ToVector3YZW(const Vector4& v) {
            return Vector3(v[1], v[2], v[3]);
        }

        inline Vector ToVector2(const double* arr) { return Vector(arr[0], arr[1]); }
        inline Vector3 ToVector3(const double* arr) { return Vector3(arr[0], arr[1], arr[2]); }
        inline Vector4 ToVector4(const double* arr) { return Vector4(arr[0], arr[1], arr[2], arr[3]); }

        template<typename T>
        inline Vector ToVector2(const T& s) { return Vector(static_cast<double>(s)); }

        template<typename T>
        inline Vector3 ToVector3(const T& s) { return Vector3(static_cast<double>(s)); }

        template<typename T>
        inline Vector4 ToVector4(const T& s) { return Vector4(static_cast<double>(s)); }


        // 创建旋转四元数
        Vector4 MakeQuaternion(const Vector3& axis, double theta);
    }
}

