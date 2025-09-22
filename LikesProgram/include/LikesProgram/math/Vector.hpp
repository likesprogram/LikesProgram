#pragma once
#include "../LikesProgramLibExport.hpp"
#include <ostream>

namespace LikesProgram {
	namespace Math {
		class LIKESPROGRAM_API Vector {
		public:
			constexpr Vector() : m_x(0.0), m_y(0.0) {}
            constexpr Vector(double x, double y) : m_x(x), m_y(y) {}
			explicit constexpr Vector(double s) : m_x(s), m_y(s) {}

			// 运算符重载
			Vector operator+(double s) const;
			Vector operator-(double s) const;
			Vector operator*(double s) const;
			Vector operator/(double s) const;

			Vector operator+(const Vector& o) const;
			Vector operator-(const Vector& o) const;
			Vector operator*(const Vector& v) const;   // 分量乘
			Vector operator/(const Vector& v) const;   // 分量除

			Vector& operator+=(double s);
			Vector& operator-=(double s);
			Vector& operator*=(double s);
			Vector& operator/=(double s);

			Vector& operator+=(const Vector& v);
			Vector& operator-=(const Vector& v);
			Vector& operator*=(const Vector& v);       // 分量乘复合赋值
			Vector& operator/=(const Vector& v);       // 分量除复合赋值

			// 一元运算
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

			// 友元
			friend Vector operator*(double s, const Vector& v);
			friend std::ostream& operator<<(std::ostream& os, const Vector& v);

			// 向量长度（模）
			double Length() const;
			// 向量长度平方
			double LengthSquared() const;

            // 向量归一化
			Vector Normalized() const;
			// 就地归一化
			void Normalize();

			// 点积（scalar product）
			double Dot(const Vector& v) const;
            // 点积（静态版本）
			static double Dot(const Vector& a, const Vector& b);

			// 向量距离
			double Distance(const Vector& v) const;
			// 向量距离平方
            double DistanceSquared(const Vector& v) const;

			// 逆时针旋转 angle 弧度
			Vector Rotated(double angle) const;
            // 逆时针旋转 angle 弧度，绕 p 旋转
			Vector RotatedAround(const Vector& p, double angle) const;

			// 判断是否为零向量，epsilon 误差
            bool IsZero(double epsilon = 1e-9) const;

			// 垂直向量（逆时针旋转 90°）
            Vector Perpendicular() const;

			// 将向量长度限制在 maxLength 内（仅当长度超出时才缩放）
            Vector Clamped(double maxLength) const;
            // 设置长度
			Vector WithLength(double length) const;

			// 2D 叉积（返回标量，值 = x1*y2 - y1*x2）
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

			// 判断当前向量与 v 是否“几乎相等”（误差不超过 epsilon）
			bool NearlyEquals(const Vector& v, double epsilon = 1e-9) const;

			// 判断向量长度是否接近 1（在 epsilon 误差范围内）
			bool IsNormalized(double epsilon = 1e-9) const;

			// 返回当前向量到 v 的有符号角度（弧度，范围通常为 [-π, π]）
			double SignedAngle(const Vector& v) const;

			// 返回每个分量取绝对值后的新向量
			Vector Abs() const;

			// 返回按分量取最小值组成的新向量
			Vector Min(const Vector& v) const;

			// 返回按分量取最大值组成的新向量
			Vector Max(const Vector& v) const;

			// 安全归一化：当长度小于 epsilon 时返回零向量，否则返回单位向量
			Vector SafeNormalized(double epsilon = 1e-9) const;

			// 极坐标构造
			static Vector FromPolar(double length, double angle);
			// 插值
			static Vector Lerp(const Vector& a, const Vector& b, double t);
			// 圆弧插值（Slerp，要求 a、b 非零，t ∈ [0,1]）
            static Vector Slerp(const Vector& a, const Vector& b, double t);

			// 零向量
            static Vector Zero();
			// 全 1 向量 (1,1)
            static Vector One();
            // X轴单位向量
            static Vector UnitX();
            // Y轴单位向量
            static Vector UnitY();
		private:
			double m_x, m_y;
		};
	}
}
