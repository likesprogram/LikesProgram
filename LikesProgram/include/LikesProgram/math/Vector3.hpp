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

            // ���������
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

            // һԪ����
            Vector3 operator-() const;
            Vector3 operator+() const;

            // �����Լ�
            Vector3& operator++();       // ǰ��++
            Vector3 operator++(int);     // ����++
            Vector3& operator--();       // ǰ��--
            Vector3 operator--(int);     // ����--

            // �Ƚ������
            bool operator==(const Vector3& v) const;
            bool operator!=(const Vector3& v) const;
            bool operator<(const Vector3& v) const;  // �ֵ���Ƚ�
            bool operator>(const Vector3& v) const;
            bool operator<=(const Vector3& v) const;
            bool operator>=(const Vector3& v) const;

            // �±����
            double& operator[](size_t i);
            const double& operator[](size_t i) const;

            // ��Ԫ
            friend Vector3 operator*(double s, const Vector3& v);
            friend std::ostream& operator<<(std::ostream& os, const Vector3& v);

            // �������ȣ�ģ��
            double Length() const;
            // ��������ƽ��
            double LengthSquared() const;
            
            // ������һ��
            Vector3 Normalized() const;
            // �͵ع�һ��
            void Normalize();
            // ��ȫ��һ��
            Vector3 SafeNormalized(double epsilon = 1e-9) const;

            // ���
            double Dot(const Vector3& v) const;
            // �������̬�汾��
            static double Dot(const Vector3& a, const Vector3& b);
            // ���
            Vector3 Cross(const Vector3& v) const;

            // ����
            double Distance(const Vector3& v) const;
            // ����ƽ��
            double DistanceSquared(const Vector3& v) const;

            // ������
            bool IsZero(double epsilon = 1e-9) const;

            // �жϵ�ǰ������ v �Ƿ񡰼�����ȡ��������� epsilon��
            bool NearlyEquals(const Vector3& v, double epsilon = 1e-9) const;

            // �ж����������Ƿ�ӽ� 1
            bool IsNormalized(double epsilon = 1e-9) const;

            // ͶӰ
            Vector3 Project(const Vector3& on) const;

            // ��������ȥ��ͶӰ���֣�
            Vector3 Reject(const Vector3& on) const;

            // ����ڵ�λ���ߵķ���
            Vector3 Reflected(const Vector3& normal) const;

            // �����ⵥλ����ת
            Vector3 RotatedAroundAxis(const Vector3& axis, double angle) const;
            // ����Ԫ����ת
            Vector3 RotatedByQuaternion(const Vector4& quat) const;

            // ����ÿ������ȡ����ֵ���������
            Vector3 Abs() const;
            
            // ���ذ�����ȡ��Сֵ��ɵ�������
            Vector3 Min(const Vector3& v) const;
            
            // ���ذ�����ȡ���ֵ��ɵ�������
            Vector3 Max(const Vector3& v) const;
            
            // ��ֵ
            static Vector3 Lerp(const Vector3& a, const Vector3& b, double t);
            // �����ֵ
            static Vector3 Slerp(const Vector3& a, const Vector3& b, double t);

            // ��̬����
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