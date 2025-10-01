#include "../../../include/LikesProgram/math/Vector4.hpp"
#include "../../../include/LikesProgram/math/Math.hpp"
#include <cmath>
#include <stdexcept>

namespace LikesProgram {
    namespace Math {

        // 算术运算
        Vector4 Vector4::operator+(double s) const {
            return { m_x + s, m_y + s, m_z + s, m_w + s };
        }
        Vector4 Vector4::operator-(double s) const {
            return { m_x - s, m_y - s, m_z - s, m_w - s };
        }
        Vector4 Vector4::operator*(double s) const {
            return { m_x * s, m_y * s, m_z * s, m_w * s };
        }
        Vector4 Vector4::operator/(double s) const {
            return { m_x / s, m_y / s, m_z / s, m_w / s };
        }

        Vector4 Vector4::operator+(const Vector4& v) const {
            return { m_x + v.m_x, m_y + v.m_y, m_z + v.m_z, m_w + v.m_w }; 
        }
        Vector4 Vector4::operator-(const Vector4& v) const {
            return { m_x - v.m_x, m_y - v.m_y, m_z - v.m_z, m_w - v.m_w }; 
        }
        Vector4 Vector4::operator*(const Vector4& v) const {
            return { m_x * v.m_x, m_y * v.m_y, m_z * v.m_z, m_w * v.m_w }; 
        }
        Vector4 Vector4::operator/(const Vector4& v) const {
            return { m_x / v.m_x, m_y / v.m_y, m_z / v.m_z, m_w / v.m_w }; 
        }

        Vector4& Vector4::operator+=(double s) {
            m_x += s; m_y += s; m_z += s; m_w += s; return *this;
        }
        Vector4& Vector4::operator-=(double s) {
            m_x -= s; m_y -= s; m_z -= s; m_w -= s; return *this;
        }
        Vector4& Vector4::operator*=(double s) {
            m_x *= s; m_y *= s; m_z *= s; m_w *= s; return *this;
        }
        Vector4& Vector4::operator/=(double s) {
            m_x /= s; m_y /= s; m_z /= s; m_w /= s; return *this;
        }

        Vector4& Vector4::operator+=(const Vector4& v) {
            m_x += v.m_x;
            m_y += v.m_y;
            m_z += v.m_z;
            m_w += v.m_w;
            return *this;
        }
        Vector4& Vector4::operator-=(const Vector4& v) {
            m_x -= v.m_x;
            m_y -= v.m_y;
            m_z -= v.m_z;
            m_w -= v.m_w;
            return *this;
        }
        Vector4& Vector4::operator*=(const Vector4& v) {
            m_x *= v.m_x;
            m_y *= v.m_y;
            m_z *= v.m_z;
            m_w *= v.m_w;
            return *this;
        }
        Vector4& Vector4::operator/=(const Vector4& v) {
            m_x /= v.m_x;
            m_y /= v.m_y;
            m_z /= v.m_z;
            m_w /= v.m_w;
            return *this;
        }

        // 一元运算
        Vector4 Vector4::operator-() const {
            return { -m_x, -m_y, -m_z, -m_w }; 
        }
        Vector4 Vector4::operator+() const {
            return *this;
        }

        // 自增自减
        Vector4& Vector4::operator++() {
            ++m_x; ++m_y; ++m_z; ++m_w; return *this;
        }
        Vector4 Vector4::operator++(int) {
            Vector4 tmp = *this; ++(*this); return tmp;
        }
        Vector4& Vector4::operator--() {
            --m_x; --m_y; --m_z; --m_w; return *this;
        }
        Vector4 Vector4::operator--(int) {
            Vector4 tmp = *this; --(*this); return tmp;
        }

        // 比较运算符
        bool Vector4::operator==(const Vector4& v) const {
            return std::abs(m_x - v.m_x) < Math::EPSILON && std::abs(m_y - v.m_y) < Math::EPSILON && std::abs(m_z - v.m_z) < Math::EPSILON && std::abs(m_w - v.m_w) < Math::EPSILON;
        }
        bool Vector4::operator!=(const Vector4& v) const {
            return !(*this == v); }
        bool Vector4::operator<(const Vector4& v) const {
            return (m_x < v.m_x) || (m_x == v.m_x && m_y < v.m_y) || (m_x == v.m_x && m_y == v.m_y && m_z < v.m_z) || (m_x == v.m_x && m_y == v.m_y && m_z == v.m_z && m_w < v.m_w);
        }
        bool Vector4::operator>(const Vector4& v) const { 
            return v < *this;
        }
        bool Vector4::operator<=(const Vector4& v) const {
            return !(*this > v);
        }
        bool Vector4::operator>=(const Vector4& v) const { 
            return !(*this < v); 
        }

        // 下标访问
        double& Vector4::operator[](size_t i) {
            if (i == 0) return m_x;
            if (i == 1) return m_y;
            if (i == 2) return m_z;
            if (i == 3) return m_w;
            throw std::out_of_range("Vector4 index out of range");
        }
        const double& Vector4::operator[](size_t i) const {
            if (i == 0) return m_x;
            if (i == 1) return m_y;
            if (i == 2) return m_z;
            if (i == 3) return m_w;
            throw std::out_of_range("Vector4 index out of range");
        }

        // 友元
        Vector4 operator*(double s, const Vector4& v) { return v * s; }
        std::ostream& operator<<(std::ostream& os, const Vector4& v) {
            return os << "(" << v[0] << "," << v[1] << "," << v[2] << "," << v[3] << ")";
        }

        // 长度/归一化
        double Vector4::Length() const { 
            return std::sqrt(LengthSquared());
        }
        double Vector4::LengthSquared() const {
            return m_x * m_x + m_y * m_y + m_z * m_z + m_w * m_w;
        }
        Vector4 Vector4::Normalized() const {
            double len = Length(); return len < Math::EPSILON ? Vector4() : *this / len;
        }
        void Vector4::Normalize() {
            double len = Length(); if (len > Math::EPSILON)*this /= len;
        }
        Vector4 Vector4::SafeNormalized(double epsilon) const { 
            double len2 = LengthSquared(); if (len2 <= epsilon * epsilon) return Vector4(); return *this * (1.0 / std::sqrt(len2));
        }

        // 点积
        double Vector4::Dot(const Vector4& v) const {
            return m_x * v.m_x + m_y * v.m_y + m_z * v.m_z + m_w * v.m_w;
        }
        double Vector4::Dot(const Vector4& a, const Vector4& b) {
            return a.Dot(b);
        }

        // 判断
        bool Vector4::IsZero(double epsilon) const {
            return std::abs(m_x) <= epsilon && std::abs(m_y) <= epsilon && std::abs(m_z) <= epsilon && std::abs(m_w) <= epsilon;
        }
        bool Vector4::NearlyEquals(const Vector4& v, double epsilon) const {
            return std::abs(m_x - v.m_x) <= epsilon && std::abs(m_y - v.m_y) <= epsilon && std::abs(m_z - v.m_z) <= epsilon && std::abs(m_w - v.m_w) <= epsilon;
        }
        bool Vector4::IsNormalized(double epsilon) const {
            return std::abs(Length() - 1.0) <= epsilon;
        }

        // 插值
        Vector4 Vector4::Lerp(const Vector4& a, const Vector4& b, double t) {
            return a + (b - a) * t;
        }
        Vector4 Vector4::Slerp(const Vector4& a, const Vector4& b, double t) {
            Vector4 na = a.SafeNormalized();
            Vector4 nb = b.SafeNormalized();
            double dot = Dot(na, nb);
            if (dot < 0.0) { nb = nb * -1.0; dot = -dot; }
            if (dot > 0.9995) return Lerp(na, nb, t).SafeNormalized();
            double theta = std::acos(dot);
            double sinTheta = std::sin(theta);
            return na * (std::sin((1 - t) * theta) / sinTheta) + nb * (std::sin(t * theta) / sinTheta);
        }

        // 静态向量
        Vector4 Vector4::Zero() {
            return { 0.0,0.0,0.0,0.0 };
        }
        Vector4 Vector4::One() {
            return { 1.0,1.0,1.0,1.0 };
        }
        Vector4 Vector4::UnitX() {
            return { 1.0,0.0,0.0,0.0 };
        }
        Vector4 Vector4::UnitY() {
            return { 0.0,1.0,0.0,0.0 };
        }
        Vector4 Vector4::UnitZ() {
            return { 0.0,0.0,1.0,0.0 };
        }
        Vector4 Vector4::UnitW() {
            return { 0.0,0.0,0.0,1.0 };
        }
    }
}
