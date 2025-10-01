#pragma once
#include "../LikesProgramLibExport.hpp"
#include "Vector4.hpp"
#include <ostream>

namespace LikesProgram {
	namespace Math {
        class LIKESPROGRAM_API Vector3 {
		public:
			constexpr Vector3() : m_x(0.0), m_y(0.0), m_z(0.0) {}
			constexpr Vector3(double x, double y, double z) : m_x(x), m_y(y), m_z(z) {}
			explicit constexpr Vector3(double s) : m_x(s), m_y(s), m_z(s) {}

            // 运算符重载
            Vector3 operator+(double s) const;
            Vector3 operator-(double s) const;
            Vector3 operator*(double s) const;
            Vector3 operator/(double s) const;

            Vector3 operator+(const Vector3& v) const;
            Vector3 operator-(const Vector3& v) const;
            Vector3 operator*(const Vector3& v) const;
            Vector3 operator/(const Vector3& v) const;

            Vector3& operator+=(double s);
            Vector3& operator-=(double s);
            Vector3& operator*=(double s);
            Vector3& operator/=(double s);

            Vector3& operator+=(const Vector3& v);
            Vector3& operator-=(const Vector3& v);
            Vector3& operator*=(const Vector3& v);
            Vector3& operator/=(const Vector3& v);

            // 一元运算
            Vector3 operator-() const;
            Vector3 operator+() const;

            // 自增自减
            Vector3& operator++();       // 前置++
            Vector3 operator++(int);     // 后置++
            Vector3& operator--();       // 前置--
            Vector3 operator--(int);     // 后置--

            // 比较运算符
            bool operator==(const Vector3& v) const;
            bool operator!=(const Vector3& v) const;
            bool operator<(const Vector3& v) const;  // 字典序比较
            bool operator>(const Vector3& v) const;
            bool operator<=(const Vector3& v) const;
            bool operator>=(const Vector3& v) const;

            // 下标访问
            double& operator[](size_t i);
            const double& operator[](size_t i) const;

            // 友元
            friend Vector3 operator*(double s, const Vector3& v);
            friend std::ostream& operator<<(std::ostream& os, const Vector3& v);

            // 向量长度（模）
            double Length() const;
            // 向量长度平方
            double LengthSquared() const;
            
            // 向量归一化
            Vector3 Normalized() const;
            // 就地归一化
            void Normalize();
            // 安全归一化
            Vector3 SafeNormalized(double epsilon = 1e-9) const;

            // 点积
            double Dot(const Vector3& v) const;
            // 点积（静态版本）
            static double Dot(const Vector3& a, const Vector3& b);
            // 叉积
            Vector3 Cross(const Vector3& v) const;

            // 距离
            double Distance(const Vector3& v) const;
            // 距离平方
            double DistanceSquared(const Vector3& v) const;

            // 零向量
            bool IsZero(double epsilon = 1e-9) const;

            // 判断当前向量与 v 是否“几乎相等”（误差不超过 epsilon）
            bool NearlyEquals(const Vector3& v, double epsilon = 1e-9) const;

            // 判断向量长度是否接近 1
            bool IsNormalized(double epsilon = 1e-9) const;

            // 投影
            Vector3 Project(const Vector3& on) const;

            // 拒向量（去掉投影部分）
            Vector3 Reject(const Vector3& on) const;

            // 相对于单位法线的反射
            Vector3 Reflected(const Vector3& normal) const;

            // 绕任意单位轴旋转
            Vector3 RotatedAroundAxis(const Vector3& axis, double angle) const;
            // 绕四元数旋转
            Vector3 RotatedByQuaternion(const Vector4& quat) const;

            // 返回每个分量取绝对值后的新向量
            Vector3 Abs() const;
            
            // 返回按分量取最小值组成的新向量
            Vector3 Min(const Vector3& v) const;
            
            // 返回按分量取最大值组成的新向量
            Vector3 Max(const Vector3& v) const;
            
            // 插值
            static Vector3 Lerp(const Vector3& a, const Vector3& b, double t);
            // 球面插值
            static Vector3 Slerp(const Vector3& a, const Vector3& b, double t);

            // 静态向量
            static Vector3 Zero();
            static Vector3 One();
            static Vector3 UnitX();
            static Vector3 UnitY();
            static Vector3 UnitZ();
		private:
            double m_x, m_y, m_z;
        };
	}
}