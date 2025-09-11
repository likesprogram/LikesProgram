#include "../../../include/LikesProgram/math/Vector.hpp"
#include "../../../include/LikesProgram/math/Math.hpp"
#include <cmath>
#include <stdexcept>

namespace LikesProgram {
    namespace Math {
        Vector Vector::operator+(double s) const {
            return { m_x + s, m_y + s };
        }
        Vector Vector::operator-(double s) const {
            return { m_x - s, m_y - s };
        }
        Vector Vector::operator+(const Vector& v) const {
            return { m_x + v.m_x, m_y + v.m_y };
        }
        Vector Vector::operator-(const Vector& v) const {
            return { m_x - v.m_x, m_y - v.m_y };
        }
        Vector Vector::operator*(double s) const {
            return { m_x * s, m_y * s };
        }
        Vector Vector::operator/(double s) const {
            return { m_x / s, m_y / s };
        }
        Vector Vector::operator*(const Vector& v) const {
            return { m_x * v.m_x, m_y * v.m_y };
        }
        Vector Vector::operator/(const Vector& v) const {
            return { m_x / v.m_x, m_y / v.m_y };
        }

        Vector& Vector::operator+=(double s) {
            m_x += s;
            m_y += s;
            return *this;
        }

        Vector& Vector::operator-=(double s) {
            m_x -= s;
            m_y -= s;
            return *this;
        }

        Vector& Vector::operator+=(const Vector& v) {
            m_x += v.m_x;
            m_y += v.m_y;
            return *this;
        }
        Vector& Vector::operator-=(const Vector& v) {
            m_x -= v.m_x;
            m_y -= v.m_y;
            return *this;
        }
        Vector& Vector::operator*=(double s) {
            m_x *= s;
            m_y *= s;
            return *this;
        }
        Vector& Vector::operator/=(double s) {
            m_x /= s;
            m_y /= s;
            return *this;
        }
        Vector& Vector::operator*=(const Vector& v) {
            m_x *= v.m_x;
            m_y *= v.m_y;
            return *this;
        }
        Vector& Vector::operator/=(const Vector& v) {
            m_x /= v.m_x;
            m_y /= v.m_y;
            return *this;
        }

        Vector Vector::operator-() const {
            return { -m_x, -m_y };
        }
        Vector Vector::operator+() const {
            return *this;
        }

        // 自增自减
        Vector& Vector::operator++() {
            ++m_x; ++m_y;
            return *this;
        }
        Vector Vector::operator++(int) {
            Vector tmp = *this;
            ++(*this);
            return tmp;
        }
        Vector& Vector::operator--() {
            --m_x; --m_y;
            return *this;
        }
        Vector Vector::operator--(int) {
            Vector tmp = *this;
            --(*this);
            return tmp;
        }

        bool Vector::operator==(const Vector& v) const {
            return std::abs(m_x - v.m_x) < Math::EPSILON &&
                std::abs(m_y - v.m_y) < Math::EPSILON;
        }
        bool Vector::operator!=(const Vector& v) const {
            return !(*this == v);
        }
        bool Vector::operator<(const Vector& v) const {
            return (m_x < v.m_x) || (m_x == v.m_x && m_y < v.m_y);
        }

        double& Vector::operator[](size_t i) {
            if (i == 0) return m_x;
            if (i == 1) return m_y;
            throw std::out_of_range("Vector index out of range");
        }
        const double& Vector::operator[](size_t i) const {
            if (i == 0) return m_x;
            if (i == 1) return m_y;
            throw std::out_of_range("Vector index out of range");
        }

        Vector operator*(double s, const Vector& v) {
            return v * s;
        }

        std::ostream& operator<<(std::ostream& os, const Vector& v) {
            return os << "(" << v.m_x << ", " << v.m_y << ")";
        }

        double Vector::Length() const {
            return std::sqrt(m_x * m_x + m_y * m_y);
        }

        Vector Vector::Normalized() const {
            double length = Length();
            if (length == 0) return Vector();
            return Vector(m_x / length, m_y / length);
        }

        double Vector::Dot(const Vector& v) const {
            return m_x * v.m_x + m_y * v.m_y;
        }

        double Vector::Distance(const Vector& v) const {
            return (*this - v).Length();
        }

        Vector Vector::Rotated(double angle) const {
            double cos = std::cos(angle), sin = std::sin(angle);
            return { cos * m_x - sin * m_y, sin * m_x + cos * m_y };
        }
        bool Vector::IsZero() const {
            return std::abs(m_x) < Math::EPSILON && std::abs(m_y) < Math::EPSILON;
        }

        Vector Vector::Perpendicular() const {
            return { -m_y, m_x };
        }

        Vector Vector::Clamped(double maxLength) const {
            double len = Length();
            if (len > maxLength) {
                return this->Normalized() * maxLength;
            }
            return *this;
        }

        double Vector::Cross(const Vector& v) const {
            return m_x * v.m_y - m_y * v.m_x;
        }

        double Vector::Angle() const {
            return std::atan2(m_y, m_x);
        }

        double Vector::AngleBetween(const Vector& v) const {
            double dot = Dot(v);
            double len2 = Length() * v.Length();
            if (len2 < Math::EPSILON) return 0.0;
            double cosTheta = dot / len2;
            cosTheta = std::max(-1.0, std::min(1.0, cosTheta)); // 避免浮点越界
            return std::acos(cosTheta);
        }

        Vector Vector::Reflected(const Vector& n) const {
            Vector norm = n.Normalized();
            return *this - 2.0 * Dot(norm) * norm;
        }

        Vector Vector::Project(const Vector& on) const {
            Vector norm = on.Normalized();
            return Dot(norm) * norm;
        }

        Vector Vector::Reject(const Vector& on) const {
            return *this - Project(on);
        }

        Vector Vector::Lerp(const Vector& a, const Vector& b, double t) {
            return a + (b - a) * t;
        }
    }
}
