#include "../../../include/LikesProgram/math/Vector3.hpp"
#include "../../../include/LikesProgram/math/Math.hpp"
#include <cmath>
#include <stdexcept>

namespace LikesProgram {
	namespace Math {
        Vector3 Vector3::operator+(double s) const {
            return { m_x + s, m_y + s, m_z + s };
        }
        Vector3 Vector3::operator-(double s) const {
            return { m_x - s, m_y - s, m_z - s };
        }
        Vector3 Vector3::operator*(double s) const {
            return { m_x * s, m_y * s, m_z * s };
        }
        Vector3 Vector3::operator/(double s) const {
            return { m_x / s, m_y / s, m_z / s };
        }

        Vector3 Vector3::operator+(const Vector3& v) const {
            return { m_x + v.m_x, m_y + v.m_y, m_z + v.m_z };
        }
        Vector3 Vector3::operator-(const Vector3& v) const {
            return { m_x - v.m_x, m_y - v.m_y, m_z - v.m_z };
        }
        Vector3 Vector3::operator*(const Vector3& v) const {
            return { m_x * v.m_x, m_y * v.m_y, m_z * v.m_z };
        }
        Vector3 Vector3::operator/(const Vector3& v) const {
            return { m_x / v.m_x, m_y / v.m_y, m_z / v.m_z };
        }

        Vector3& Vector3::operator+=(double s) {
            m_x += s;
            m_y += s;
            m_z += s;
            return *this;
        }
        Vector3& Vector3::operator-=(double s) {
            m_x -= s;
            m_y -= s;
            m_z -= s;
            return *this;
        }
        Vector3& Vector3::operator*=(double s) {
            m_x *= s;
            m_y *= s;
            m_z *= s;
            return *this;
        }
        Vector3& Vector3::operator/=(double s) {
            m_x /= s;
            m_y /= s;
            m_z /= s;
            return *this;
        }

        Vector3& Vector3::operator+=(const Vector3& v) {
            m_x += v.m_x;
            m_y += v.m_y;
            m_z += v.m_z;
            return *this;
        }
        Vector3& Vector3::operator-=(const Vector3& v) {
            m_x -= v.m_x;
            m_y -= v.m_y;
            m_z -= v.m_z;
            return *this;
        }
        Vector3& Vector3::operator*=(const Vector3& v) {
            m_x *= v.m_x;
            m_y *= v.m_y;
            m_z *= v.m_z;
            return *this;
        }
        Vector3& Vector3::operator/=(const Vector3& v) {
            m_x /= v.m_x;
            m_y /= v.m_y;
            m_z /= v.m_z;
            return *this;
        }

        Vector3 Vector3::operator-() const {
            return { -m_x, -m_y, -m_z };
        }
        Vector3 Vector3::operator+() const {
            return *this;
        }

        Vector3& Vector3::operator++() {
            ++m_x; ++m_y; ++m_z; return *this;
        }
        Vector3 Vector3::operator++(int) {
            Vector3 tmp = *this; ++(*this);
            return tmp;
        }
        Vector3& Vector3::operator--() {
            --m_x; --m_y; --m_z; return *this;
        }
        Vector3 Vector3::operator--(int) {
            Vector3 tmp = *this; --(*this); return tmp;
        }

        bool Vector3::operator==(const Vector3& v) const {
            return std::abs(m_x - v.m_x) < 1e-9 && std::abs(m_y - v.m_y) < 1e-9 && std::abs(m_z - v.m_z) < 1e-9;
        }
        bool Vector3::operator!=(const Vector3& v) const {
            return !(*this == v);
        }
        bool Vector3::operator<(const Vector3& v) const {
            return (m_x < v.m_x) || (m_x == v.m_x && m_y < v.m_y) || (m_x == v.m_x && m_y == v.m_y && m_z < v.m_z);
        }
        bool Vector3::operator>(const Vector3& v) const {
            return v < *this;
        }
        bool Vector3::operator<=(const Vector3& v) const {
            return !(*this > v);
        }
        bool Vector3::operator>=(const Vector3& v) const {
            return !(*this < v);
        }

        double& Vector3::operator[](size_t i) {
            if (i == 0) return m_x;
            if (i == 1) return m_y;
            if (i == 2) return m_z;
            throw std::out_of_range("Vector3 index out of range");
        }
        const double& Vector3::operator[](size_t i) const {
            if (i == 0) return m_x;
            if (i == 1) return m_y;
            if (i == 2) return m_z;
            throw std::out_of_range("Vector3 index out of range");
        }

        Vector3 operator*(double s, const Vector3& v) {
            return v * s;
        }
        std::ostream& operator<<(std::ostream& os, const Vector3& v) {
            return os << "(" << v[0] << ", " << v[1] << ", " << v[2] << ")";
        }

        double Vector3::Length() const {
            return std::sqrt(m_x * m_x + m_y * m_y + m_z * m_z);
        }
        double Vector3::LengthSquared() const {
            return m_x * m_x + m_y * m_y + m_z * m_z;
        }

        Vector3 Vector3::Normalized() const {
            double len = Length();
            return len < Math::EPSILON ? Vector3() : Vector3(m_x / len, m_y / len, m_z / len);
        }
        void Vector3::Normalize() {
            double len = Length();
            if (len > Math::EPSILON) {
                m_x /= len; m_y /= len; m_z /= len;
            }
        }
        Vector3 Vector3::SafeNormalized(double epsilon) const {
            double len2 = LengthSquared();
            if (len2 <= epsilon * epsilon) return Vector3();
            double inv = 1.0 / std::sqrt(len2);
            return Vector3(m_x * inv, m_y * inv, m_z * inv);
        }

        double Vector3::Dot(const Vector3& v) const {
            return m_x * v.m_x + m_y * v.m_y + m_z * v.m_z;
        }
        double Vector3::Dot(const Vector3& a, const Vector3& b) {
            return a.Dot(b);
        }
        Vector3 Vector3::Cross(const Vector3& v) const {
            return { m_y * v.m_z - m_z * v.m_y, m_z * v.m_x - m_x * v.m_z, m_x * v.m_y - m_y * v.m_x };
        }

        double Vector3::Distance(const Vector3& v) const {
            return (*this - v).Length();
        }
        double Vector3::DistanceSquared(const Vector3& v) const {
            return (*this - v).LengthSquared();
        }

        bool Vector3::IsZero(double epsilon) const {
            return std::abs(m_x) < epsilon && std::abs(m_y) < epsilon && std::abs(m_z) < epsilon;
        }
        bool Vector3::NearlyEquals(const Vector3& v, double epsilon) const {
            return std::abs(m_x - v.m_x) <= epsilon && std::abs(m_y - v.m_y) <= epsilon && std::abs(m_z - v.m_z) <= epsilon;
        }
        bool Vector3::IsNormalized(double epsilon) const {
            return std::abs(Length() - 1.0) <= epsilon;
        }

        Vector3 Vector3::Project(const Vector3& on) const {
            Vector3 n = on.SafeNormalized(); return n * Dot(n);
        }
        Vector3 Vector3::Reject(const Vector3& on) const {
            return *this - Project(on);
        }
        Vector3 Vector3::Reflected(const Vector3& normal) const {
            Vector3 n = normal.SafeNormalized(); return *this - 2 * Dot(n) * n;
        }

        Vector3 Vector3::RotatedAroundAxis(const Vector3& axis, double angle) const {
            Vector3 k = axis.SafeNormalized();
            double cosTheta = std::cos(angle);
            double sinTheta = std::sin(angle);
            return (*this) * cosTheta + k.Cross(*this) * sinTheta + k * (k.Dot(*this)) * (1 - cosTheta);
        }

        Vector3 Vector3::RotatedByQuaternion(const Vector4& quat) const {
            // q = (x, y, z, w)
            double qx = quat[0], qy = quat[1], qz = quat[2], qw = quat[3];

            // 将向量v看作四元数v' = (vx, vy, vz, 0)
            double vx = m_x, vy = m_y, vz = m_z;

            // 四元数乘法 q * v
            double ix = qw * vx + qy * vz - qz * vy;
            double iy = qw * vy + qz * vx - qx * vz;
            double iz = qw * vz + qx * vy - qy * vx;
            double iw = -qx * vx - qy * vy - qz * vz;

            // 再乘 q^-1 (单位四元数 q^-1 = q* 共轭)
            double rx = ix * qw - iw * qx - iy * qz + iz * qy;
            double ry = iy * qw - iw * qy - iz * qx + ix * qz;
            double rz = iz * qw - iw * qz - ix * qy + iy * qx;

            return Vector3(rx, ry, rz);
        }

        Vector3 Vector3::Abs() const {
            return { std::abs(m_x), std::abs(m_y), std::abs(m_z) };
        }
        Vector3 Vector3::Min(const Vector3& v) const {
            return { std::min(m_x,v.m_x), std::min(m_y,v.m_y), std::min(m_z,v.m_z) };
        }
        Vector3 Vector3::Max(const Vector3& v) const {
            return { std::max(m_x,v.m_x), std::max(m_y,v.m_y), std::max(m_z,v.m_z) };
        }

        Vector3 Vector3::Lerp(const Vector3& a, const Vector3& b, double t) {
            return a + (b - a) * t;
        }
        Vector3 Vector3::Slerp(const Vector3& a, const Vector3& b, double t) {
            if (a.IsZero() && b.IsZero()) return Vector3::Zero();
            if (a.IsZero()) return b * t;
            if (b.IsZero()) return a * (1 - t);
            Vector3 na = a.SafeNormalized(), nb = b.SafeNormalized();
            double theta = std::acos(std::max(-1.0, std::min(1.0, na.Dot(nb))));
            if (theta < 1e-6) return Lerp(na, nb, t).SafeNormalized();
            double sinTheta = std::sin(theta);
            return (std::sin((1 - t) * theta) / sinTheta) * na + (std::sin(t * theta) / sinTheta) * nb;
        }

        Vector3 Vector3::Zero() {
            return { 0,0,0 };
        }
        Vector3 Vector3::One() {
            return { 1,1,1 };
        }
        Vector3 Vector3::UnitX() {
            return { 1,0,0 };
        }
        Vector3 Vector3::UnitY() {
            return { 0,1,0 };
        }
        Vector3 Vector3::UnitZ() {
            return { 0,0,1 };
        }
	}
}