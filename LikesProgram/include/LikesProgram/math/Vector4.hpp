#pragma once
#include "../LikesProgramLibExport.hpp"
#include <ostream>

namespace LikesProgram { 
	namespace Math {
        class LIKESPROGRAM_API Vector4 {
        public:
            constexpr Vector4() : m_x(0.0), m_y(0.0), m_z(0.0), m_w(0.0) {}
            constexpr Vector4(double x, double y, double z, double w) : m_x(x), m_y(y), m_z(z), m_w(w) {}
            explicit constexpr Vector4(double s) : m_x(s), m_y(s), m_z(s), m_w(s) {}

            // 算术运算
            Vector4 operator+(double s) const;
            Vector4 operator-(double s) const;
            Vector4 operator*(double s) const;
            Vector4 operator/(double s) const;

            Vector4 operator+(const Vector4& v) const;
            Vector4 operator-(const Vector4& v) const;
            Vector4 operator*(const Vector4& v) const;
            Vector4 operator/(const Vector4& v) const;

            Vector4& operator+=(double s);
            Vector4& operator-=(double s);
            Vector4& operator*=(double s);
            Vector4& operator/=(double s);

            Vector4& operator+=(const Vector4& v);
            Vector4& operator-=(const Vector4& v);
            Vector4& operator*=(const Vector4& v);
            Vector4& operator/=(const Vector4& v);

            // 一元运算
            Vector4 operator-() const;
            Vector4 operator+() const;

            // 自增自减
            Vector4& operator++();
            Vector4 operator++(int);
            Vector4& operator--();
            Vector4 operator--(int);

            // 比较运算符
            bool operator==(const Vector4& v) const;
            bool operator!=(const Vector4& v) const;
            bool operator<(const Vector4& v) const;
            bool operator>(const Vector4& v) const;
            bool operator<=(const Vector4& v) const;
            bool operator>=(const Vector4& v) const;

            // 下标访问
            double& operator[](size_t i);
            const double& operator[](size_t i) const;

            // 友元
            friend Vector4 operator*(double s, const Vector4& v);
            friend std::ostream& operator<<(std::ostream& os, const Vector4& v);

            // 长度
            double Length() const;
            double LengthSquared() const;

            // 归一化
            Vector4 Normalized() const;
            void Normalize();
            Vector4 SafeNormalized(double epsilon = 1e-9) const;

            // 点积
            double Dot(const Vector4& v) const;
            static double Dot(const Vector4& a, const Vector4& b);

            // 判断
            bool IsZero(double epsilon = 1e-9) const;
            bool NearlyEquals(const Vector4& v, double epsilon = 1e-9) const;
            bool IsNormalized(double epsilon = 1e-9) const;

            // 插值
            static Vector4 Lerp(const Vector4& a, const Vector4& b, double t);
            static Vector4 Slerp(const Vector4& a, const Vector4& b, double t);

            // 静态向量
            static Vector4 Zero();
            static Vector4 One();
            static Vector4 UnitX();
            static Vector4 UnitY();
            static Vector4 UnitZ();
            static Vector4 UnitW();

        private:
            double m_x, m_y, m_z, m_w;
        };
	}
}