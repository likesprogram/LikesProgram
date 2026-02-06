#pragma once
#include <iostream>
#include <random>
#include "../../include/LikesProgram/math/Math.hpp"
#include "../../include/LikesProgram/math/Vector3.hpp"
#include "../../include/LikesProgram/math/Vector4.hpp"
#include "../../include/LikesProgram/time/Timer.hpp"

namespace Vector3Test {

    void StressTest(size_t count = 100000) {
        std::cout << "\n===== Vector3 Stress Test (" << count << " 次) =====\n";

        std::mt19937 rng(std::random_device{}());
        std::uniform_real_distribution<double> dist(-1000.0, 1000.0);

        LikesProgram::Time::Timer timer(true);

        LikesProgram::Math::Vector3 acc(0.0, 0.0, 0.0);
        for (size_t i = 0; i < count; i++) {
            LikesProgram::Math::Vector3 a(dist(rng), dist(rng), dist(rng));
            LikesProgram::Math::Vector3 b(dist(rng), dist(rng), dist(rng));

            acc += a + b;
            acc -= a - b;
            acc *= 0.5;
            acc += a * b; // 分量乘
            acc -= a / (b + LikesProgram::Math::Vector3(1.0, 1.0, 1.0));
            acc += LikesProgram::Math::Vector3::Lerp(a, b, 0.5);
            acc += LikesProgram::Math::Vector3::Slerp(a, b, 0.5);
        }

        std::cout << "Stress Test 完成\n";
        std::cout << "最终累积结果: " << acc << "\n";
        std::cout << "耗时: " << LikesProgram::Time::NsToMs(timer.Stop().count()) << "ms" << std::endl;
    }

    void BasicOps() {
        std::cout << "===== Vector3 基本运算符测试 =====\n";
        LikesProgram::Math::Vector3 a(1.0, 2.0, 3.0);
        LikesProgram::Math::Vector3 b(4.0, 5.0, 6.0);

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
        std::cout << "\n===== Vector3 复合赋值测试 =====\n";
        LikesProgram::Math::Vector3 v(1.0, 2.0, 3.0);

        std::cout << "v = " << v << "\n";
        v += LikesProgram::Math::Vector3(1.0, 1.0, 1.0);
        std::cout << "v += (1,1,1) -> " << v << "\n";
        v -= LikesProgram::Math::Vector3(0.5, 0.5, 0.5);
        std::cout << "v -= (0.5,0.5,0.5) -> " << v << "\n";
        v *= 2.0;
        std::cout << "v *= 2 -> " << v << "\n";
        v /= 2.0;
        std::cout << "v /= 2 -> " << v << "\n";
    }

    void MathOps() {
        std::cout << "\n===== Vector3 数学运算测试 =====\n";
        LikesProgram::Math::Vector3 a(1.0, 2.0, 3.0);
        LikesProgram::Math::Vector3 b(4.0, 5.0, 6.0);
        LikesProgram::Math::Vector3 axis(0.0, 1.0, 0.0);

        std::cout << "a = " << a << ", b = " << b << "\n";
        std::cout << "Length(a) = " << a.Length() << "\n";
        std::cout << "a.Normalized() = " << a.Normalized() << "\n";
        std::cout << "a.SafeNormalized() = " << a.SafeNormalized() << "\n";
        std::cout << "a.Dot(b) = " << a.Dot(b) << "\n";
        std::cout << "a.Cross(b) = " << a.Cross(b) << "\n";
        std::cout << "a.Distance(b) = " << a.Distance(b) << "\n";
        std::cout << "a.Abs() = " << a.Abs() << "\n";
        std::cout << "a.Min(b) = " << a.Min(b) << ", a.Max(b) = " << a.Max(b) << "\n";

        std::cout << "a.Project(b) = " << a.Project(b) << "\n";
        std::cout << "a.Reject(b) = " << a.Reject(b) << "\n";
        std::cout << "a.Reflected(b.Normalized()) = " << a.Reflected(b.Normalized()) << "\n";

        std::cout << "a.RotatedAroundAxis(axis, pi/2) = "
            << a.RotatedAroundAxis(axis, LikesProgram::Math::PI / 2) << "\n";

        LikesProgram::Math::Vector4 quat = LikesProgram::Math::MakeQuaternion(axis, LikesProgram::Math::PI / 2);
        std::cout << "a.RotatedByQuaternion(quat) = " << a.RotatedByQuaternion(quat) << "\n";
    }

    void InterpolationTest() {
        std::cout << "\n===== Vector3 插值测试 =====\n";
        LikesProgram::Math::Vector3 a(0.0, 0.0, 0.0);
        LikesProgram::Math::Vector3 b(1.0, 1.0, 1.0);

        for (double t = 0.0; t <= 1.0; t += 0.25) {
            std::cout << "Lerp(a,b," << t << ") = " << LikesProgram::Math::Vector3::Lerp(a, b, t) << "\n";
            std::cout << "Slerp(a,b," << t << ") = " << LikesProgram::Math::Vector3::Slerp(a, b, t) << "\n";
        }
    }

    void Test() {
        BasicOps();
        CompoundOps();
        MathOps();
        InterpolationTest();
        StressTest();
    }

}
