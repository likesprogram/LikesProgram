#pragma once
#include "../LikesProgramLibExport.hpp"
#include <ostream>

namespace LikesProgram {
	namespace Math {
		class LIKESPROGRAM_API Vector {
		public:
			constexpr Vector() : m_x(0), m_y(0) {}
            constexpr Vector(double x, double y) : m_x(x), m_y(y) {}
			explicit constexpr Vector(double s) : m_x(s), m_y(s) {}

			// 运算符重载
			Vector operator+(double s) const;
			Vector operator-(double s) const;
			Vector operator+(const Vector& o) const;
			Vector operator-(const Vector& o) const;
			Vector operator*(double s) const;
			Vector operator/(double s) const;
			Vector operator*(const Vector& v) const;   // 分量乘
			Vector operator/(const Vector& v) const;   // 分量除

			Vector& operator+=(double s);
			Vector& operator-=(double s);
			Vector& operator+=(const Vector& v);
			Vector& operator-=(const Vector& v);
			Vector& operator*=(double s);
			Vector& operator/=(double s);
			Vector& operator*=(const Vector& v);       // 分量乘复合赋值
			Vector& operator/=(const Vector& v);       // 分量除复合赋值

			Vector operator-() const;   // 取负
			Vector operator+() const;   // 取正

			// 自增自减
			Vector& operator++();       // 前置 ++
			Vector operator++(int);     // 后置 ++
			Vector& operator--();       // 前置 --
			Vector operator--(int);     // 后置 --

			bool operator==(const Vector& v) const;
			bool operator!=(const Vector& v) const;
			bool operator<(const Vector& v) const;  // 字典序比较
			bool operator>(const Vector& v) const { return v < *this; }
			bool operator<=(const Vector& v) const { return !(v < *this); }
			bool operator>=(const Vector& v) const { return !(*this < v); }

			double& operator[](size_t i);
			const double& operator[](size_t i) const;

			friend Vector operator*(double s, const Vector& v);
			friend std::ostream& operator<<(std::ostream& os, const Vector& v);

			// 向量长度（模）
			double Length() const;

            // 单位向量
			Vector Normalized() const;

			// 向量积（点积）
			double Dot(const Vector& v) const;

			// 向量距离
			double Distance(const Vector& v) const;

			// 逆时针旋转 angle 弧度
			Vector Rotated(double angle) const;

			// 判断是否为零向量
            bool IsZero() const;

			// 垂直向量
            Vector Perpendicular() const;

			// 限制长度
            Vector Clamped(double maxLength) const;

			// 2D 叉积（返回标量）
			double Cross(const Vector& v) const;

			// 返回极角 atan2(y,x)
			double Angle() const;

			// 返回夹角 [0,pi]
			double AngleBetween(const Vector& v) const;

			// 相对于单位法线的反射
			Vector Reflected(const Vector& normal) const;

			// 投影到某个向量上
			Vector Project(const Vector& on) const;

			// 拒向量（去掉投影部分）
			Vector Reject(const Vector& on) const;

			// 插值
			static Vector Lerp(const Vector& a, const Vector& b, double t);
		private:
			double m_x, m_y;
		};
	}
}

