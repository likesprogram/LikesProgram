#pragma once
#include <iostream>
#include "../LikesProgram/math/Vector.hpp"
#include "../LikesProgram/math/Math.hpp"
#include "../LikesProgram/Timer.hpp"
#include <random>

namespace VectorTest {
    void StressTest(size_t count = 100000) {
        std::cout << "\n===== 随机 Stress Test (" << count << " 次) =====\n";

        std::mt19937 rng(std::random_device{}());
        std::uniform_real_distribution<double> dist(-1000.0, 1000.0);

        LikesProgram::Timer timer(true);

        LikesProgram::Math::Vector acc(0.0, 0.0);
        for (size_t i = 0; i < count; i++) {
            LikesProgram::Math::Vector a(dist(rng), dist(rng));
            LikesProgram::Math::Vector b(dist(rng), dist(rng));

            acc += a + b;
            acc -= a - b;
            acc *= 0.5;
            acc += a * b; // 分量乘
            acc -= a / (b + LikesProgram::Math::Vector(1.0, 1.0));
            acc += a.Normalized().Clamped(10.0);
            acc += LikesProgram::Math::Vector::Lerp(a, b, 0.5);
        }

        std::cout << "Stress Test 完成\n";
        std::cout << "最终累积结果: " << acc << "\n";
        std::cout << "耗时: " << LikesProgram::Timer::ToString(timer.Stop()) << "\n";
    }

    void BasicOps() {
        std::cout << "===== 基本运算符测试 =====\n";
        LikesProgram::Math::Vector a(3.0, 4.0);
        LikesProgram::Math::Vector b(1.0, 2.0);

        std::cout << "a = " << a << ", b = " << b << "\n";
        std::cout << "a + b = " << (a + b) << "\n";
        std::cout << "a - b = " << (a - b) << "\n";
        std::cout << "a * 2 = " << (a * 2.0) << "\n";
        std::cout << "2 * a = " << (2.0 * a) << "\n";
        std::cout << "a / 2 = " << (a / 2.0) << "\n";
        std::cout << "a * b (分量乘) = " << (a * b) << "\n";
        std::cout << "a / b (分量除) = " << (a / b) << "\n";
        std::cout << "-a = " << (-a) << "\n";
        std::cout << "+a = " << (+a) << "\n";
    }

    void CompoundOps() {
        std::cout << "\n===== 复合赋值测试 =====\n";
        LikesProgram::Math::Vector v(2.0, 3.0);
        std::cout << "v = " << v << "\n";
        v += LikesProgram::Math::Vector(1.0, 1.0);
        std::cout << "v += (1,1) -> " << v << "\n";
        v -= LikesProgram::Math::Vector(0.5, 0.5);
        std::cout << "v -= (0.5,0.5) -> " << v << "\n";
        v *= 2.0;
        std::cout << "v *= 2 -> " << v << "\n";
        v /= 2.0;
        std::cout << "v /= 2 -> " << v << "\n";
    }

    void MathOps() {
        std::cout << "\n===== 数学运算测试 =====\n";
        LikesProgram::Math::Vector a(3.0, 4.0);
        LikesProgram::Math::Vector b(1.0, 0.0);
        LikesProgram::Math::Vector n(0.0, 1.0);

        std::cout << "a = " << a << ", b = " << b << ", n = " << n << "\n";
        std::cout << "Length(a) = " << a.Length() << "\n";
        std::cout << "a.Normalized() = " << a.Normalized() << "\n";
        std::cout << "a.SafeNormalized() = " << a.SafeNormalized() << "\n";
        std::cout << "a.WithLength(10) = " << a.WithLength(10) << "\n";
        std::cout << "a.Clamped(2) = " << a.Clamped(2) << "\n";
        std::cout << "a.Cross(b) = " << a.Cross(b) << "\n";
        std::cout << "a.Dot(b) = " << a.Dot(b) << "\n";
        std::cout << "a.Distance(b) = " << a.Distance(b) << "\n";
        std::cout << "a.Abs() = " << a.Abs() << "\n";
        std::cout << "a.Min(b) = " << a.Min(b) << ", a.Max(b) = " << a.Max(b) << "\n";
        std::cout << "a.NearlyEquals(b) = " << a.NearlyEquals(b) << "\n";


        std::cout << "Angle(a) = " << a.Angle() << "\n";
        std::cout << "AngleBetween(a,b) = " << a.AngleBetween(b) << "\n";
        std::cout << "SignedAngle(a,b) = " << a.SignedAngle(b) << "\n";
        std::cout << "Rotated(a, pi/2) = " << a.Rotated(LikesProgram::Math::PI / 2) << "\n";
        std::cout << "RotatedAround(a, b, pi/2) = " << a.RotatedAround(b, LikesProgram::Math::PI / 2) << "\n";


        std::cout << "Perpendicular(a) = " << a.Perpendicular() << "\n";
        std::cout << "Reflected(a,n) = " << a.Reflected(n) << "\n";
        std::cout << "Project(a,n) = " << a.Project(n) << "\n";
        std::cout << "Reject(a,n) = " << a.Reject(n) << "\n";
        std::cout << "FromPolar(5, pi/4) = " << LikesProgram::Math::Vector::FromPolar(5, LikesProgram::Math::PI / 4) << "\n";
    }

    void LerpTest() {
        std::cout << "\n===== 插值测试 =====\n";
        LikesProgram::Math::Vector a(0.0, 0.0);
        LikesProgram::Math::Vector b(10.0, 10.0);

        for (double t = 0.0; t <= 1.0; t += 0.25) {
            std::cout << "Lerp(a,b," << t << ") = " << LikesProgram::Math::Vector::Lerp(a, b, t) << "\n";
            std::cout << "Slerp(a,b," << t << ") = " << LikesProgram::Math::Vector::Slerp(a, b, t) << "\n";
        }
    }

    void Test() {
        BasicOps();
        CompoundOps();
        MathOps();
        LerpTest();
        StressTest();
    }
}
