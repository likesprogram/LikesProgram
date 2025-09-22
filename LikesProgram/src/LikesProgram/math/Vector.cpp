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
        Vector Vector::operator*(double s) const {
            return { m_x * s, m_y * s };
        }
        Vector Vector::operator/(double s) const {
            return { m_x / s, m_y / s };
        }

        Vector Vector::operator+(const Vector& v) const {
            return { m_x + v.m_x, m_y + v.m_y };
        }
        Vector Vector::operator-(const Vector& v) const {
            return { m_x - v.m_x, m_y - v.m_y };
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

        double Vector::LengthSquared() const {
            return m_x * m_x + m_y * m_y;
        }

        Vector Vector::Normalized() const {
            double length = Length();
            if (length <= Math::EPSILON) return Vector();
            return Vector(m_x / length, m_y / length);
        }

        void Vector::Normalize() {
            double length = Length();
            if (length > Math::EPSILON) {
                m_x /= length;
                m_y /= length;
            }
        }

        double Vector::Dot(const Vector& v) const {
            return m_x * v.m_x + m_y * v.m_y;
        }

        double Vector::Dot(const Vector& a, const Vector& b) {
            return a.m_x * b.m_x + a.m_y * b.m_y;
        }

        double Vector::Distance(const Vector& v) const {
            return (*this - v).Length();
        }

        double Vector::DistanceSquared(const Vector& v) const {
            double dx = m_x - v.m_x;
            double dy = m_y - v.m_y;
            return dx * dx + dy * dy;
        }

        Vector Vector::Rotated(double angle) const {
            double cos = std::cos(angle), sin = std::sin(angle);
            return { cos * m_x - sin * m_y, sin * m_x + cos * m_y };
        }
        Vector Vector::RotatedAround(const Vector& p, double angle) const {
            return ((*this) - p).Rotated(angle) + p;
        }

        bool Vector::IsZero(double epsilon) const {
            return std::abs(m_x) < epsilon && std::abs(m_y) < epsilon;
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

        Vector Vector::WithLength(double length) const {
            double _length = Length();
            if (_length < Math::EPSILON) return Vector();
            return (*this) * (length / _length);
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

        bool Vector::NearlyEquals(const Vector& v, double epsilon) const {
            return (std::abs(m_x - v.m_x) <= epsilon) && (std::abs(m_y - v.m_y) <= epsilon);
        }

        bool Vector::IsNormalized(double epsilon) const {
            return std::abs(Length() - 1.0) <= epsilon;
        }

        double Vector::SignedAngle(const Vector& v) const {
            return std::atan2(Cross(v), Dot(v));
        }

        Vector Vector::Abs() const {
            return Vector(std::abs(m_x), std::abs(m_y));
        }

        Vector Vector::Min(const Vector& v) const {
            return Vector(std::min(m_x, v.m_x), std::min(m_y, v.m_y));
        }

        Vector Vector::Max(const Vector& v) const {
            return Vector(std::max(m_x, v.m_x), std::max(m_y, v.m_y));
        }

        Vector Vector::SafeNormalized(double epsilon) const {
            double lengthSquared = LengthSquared();
            if (lengthSquared <= epsilon * epsilon) return Vector();
            double invLen = 1.0 / std::sqrt(lengthSquared);
            return Vector(m_x * invLen, m_y * invLen);
        }

        Vector Vector::FromPolar(double length, double angle) {
            return Vector(length * std::cos(angle), length * std::sin(angle));
        }
        Vector Vector::Lerp(const Vector& a, const Vector& b, double t) {
            return a + (b - a) * t;
        }
        Vector Vector::Slerp(const Vector& a, const Vector& b, double t) {
            if (a.IsZero() && b.IsZero()) return Vector::Zero();
            if (a.IsZero()) return b * t;
            if (b.IsZero()) return a * (1 - t);

            Vector na = a.Normalized();
            Vector nb = b.Normalized();

            double theta = na.AngleBetween(nb);

            if (theta < 1e-6) {
                return Vector::Lerp(na, nb, t).SafeNormalized();
            }

            double sinTheta = std::sin(theta);
            return (std::sin((1 - t) * theta) / sinTheta) * na +
                (std::sin(t * theta) / sinTheta) * nb;
        }

        Vector Vector::Zero() {
            return Vector(0.0, 0.0);
        }
        Vector Vector::One() {
            return Vector(1.0, 1.0);
        }
        Vector Vector::UnitX() {
            return Vector(1.0, 0.0);
        }
        Vector Vector::UnitY()
        {
            return Vector(0.0, 1.0);
        }
    }
}
