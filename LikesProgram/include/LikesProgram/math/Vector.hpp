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

			// ���������
			Vector operator+(double s) const;
			Vector operator-(double s) const;
			Vector operator+(const Vector& o) const;
			Vector operator-(const Vector& o) const;
			Vector operator*(double s) const;
			Vector operator/(double s) const;
			Vector operator*(const Vector& v) const;   // ������
			Vector operator/(const Vector& v) const;   // ������

			Vector& operator+=(double s);
			Vector& operator-=(double s);
			Vector& operator+=(const Vector& v);
			Vector& operator-=(const Vector& v);
			Vector& operator*=(double s);
			Vector& operator/=(double s);
			Vector& operator*=(const Vector& v);       // �����˸��ϸ�ֵ
			Vector& operator/=(const Vector& v);       // ���������ϸ�ֵ

			Vector operator-() const;   // ȡ��
			Vector operator+() const;   // ȡ��

			// �����Լ�
			Vector& operator++();       // ǰ�� ++
			Vector operator++(int);     // ���� ++
			Vector& operator--();       // ǰ�� --
			Vector operator--(int);     // ���� --

			bool operator==(const Vector& v) const;
			bool operator!=(const Vector& v) const;
			bool operator<(const Vector& v) const;  // �ֵ���Ƚ�
			bool operator>(const Vector& v) const { return v < *this; }
			bool operator<=(const Vector& v) const { return !(v < *this); }
			bool operator>=(const Vector& v) const { return !(*this < v); }

			double& operator[](size_t i);
			const double& operator[](size_t i) const;

			friend Vector operator*(double s, const Vector& v);
			friend std::ostream& operator<<(std::ostream& os, const Vector& v);

			// �������ȣ�ģ��
			double Length() const;

            // ��λ����
			Vector Normalized() const;

			// �������������
			double Dot(const Vector& v) const;

			// ��������
			double Distance(const Vector& v) const;

			// ��ʱ����ת angle ����
			Vector Rotated(double angle) const;

			// �ж��Ƿ�Ϊ������
            bool IsZero() const;

			// ��ֱ����
            Vector Perpendicular() const;

			// ���Ƴ���
            Vector Clamped(double maxLength) const;

			// 2D ��������ر�����
			double Cross(const Vector& v) const;

			// ���ؼ��� atan2(y,x)
			double Angle() const;

			// ���ؼн� [0,pi]
			double AngleBetween(const Vector& v) const;

			// ����ڵ�λ���ߵķ���
			Vector Reflected(const Vector& normal) const;

			// ͶӰ��ĳ��������
			Vector Project(const Vector& on) const;

			// ��������ȥ��ͶӰ���֣�
			Vector Reject(const Vector& on) const;

			// ��ֵ
			static Vector Lerp(const Vector& a, const Vector& b, double t);
		private:
			double m_x, m_y;
		};
	}
}

