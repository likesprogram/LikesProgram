#pragma once
#include <iostream>
#include "../LikesProgram/math/Vector.hpp"
#include "../LikesProgram/math/Math.hpp"
#include "../LikesProgram/Timer.hpp"
#include <random>

namespace VectorTest {
    void StressTest(size_t count = 100000) {
        std::cout << std::endl << "===== 随机 Stress Test (" << count << " 次) =====" << std::endl;

        std::mt19937 rng(std::random_device{}());
        std::uniform_real_distribution<double> dist(-1000.0, 1000.0);

        //LikesProgram::Timer::Start(); // 开始计时

        LikesProgram::Math::Vector acc(0.0, 0.0); // 累积结果
        for (size_t i = 0; i < count; i++) {
            LikesProgram::Math::Vector a(dist(rng), dist(rng));
            LikesProgram::Math::Vector b(dist(rng), dist(rng));

            // 做一些运算
            acc += a + b;
            acc -= a - b;
            acc *= 0.5;
            acc += a * b;     // 分量乘
            acc -= a / (b + LikesProgram::Math::Vector(1.0, 1.0)); // 避免除 0
            acc += a.Normalized().Clamped(10.0);
            acc += LikesProgram::Math::Vector::Lerp(a, b, 0.5);
        }


        std::cout << "Stress Test 完成" << std::endl;
        std::cout << "最终累积结果: " << acc << std::endl;
        //std::cout << "耗时: " << LikesProgram::Timer::ToString(LikesProgram::Timer::Stop()) << std::endl;
    }

    void BasicOps() {
        std::cout << "===== 基本运算符测试 =====" << std::endl;
        LikesProgram::Math::Vector a(3.0, 4.0);
        LikesProgram::Math::Vector b(1.0, 2.0);

        std::cout << "a = " << a << ", b = " << b << std::endl;
        std::cout << "a + b = " << (a + b) << std::endl;
        std::cout << "a - b = " << (a - b) << std::endl;
        std::cout << "a * 2 = " << (a * 2.0) << std::endl;
        std::cout << "2 * a = " << (2.0 * a) << std::endl;
        std::cout << "a / 2 = " << (a / 2.0) << std::endl;
        std::cout << "a * b (分量乘) = " << (a * b) << std::endl;
        std::cout << "a / b (分量除) = " << (a / b) << std::endl;
        std::cout << "-a = " << (-a) << std::endl;
        std::cout << "+a = " << (+a) << std::endl;
    }

    void CompoundOps() {
        std::cout << std::endl << "===== 复合赋值测试 =====" << std::endl;
        LikesProgram::Math::Vector v(2.0, 3.0);
        std::cout << "v = " << v << std::endl;
        v += LikesProgram::Math::Vector(1.0, 1.0);
        std::cout << "v += (1,1) -> " << v << std::endl;
        v -= LikesProgram::Math::Vector(0.5, 0.5);
        std::cout << "v -= (0.5,0.5) -> " << v << std::endl;
        v *= 2.0;
        std::cout << "v *= 2 -> " << v << std::endl;
        v /= 2.0;
        std::cout << "v /= 2 -> " << v << std::endl;
    }

    void MathOps() {
        std::cout << std::endl << "===== 数学运算测试 =====" << std::endl;
        LikesProgram::Math::Vector a(3.0, 4.0);
        LikesProgram::Math::Vector b(1.0, 0.0);

        std::cout << "a = " << a << ", b = " << b << std::endl;
        std::cout << "Length(a) = " << a.Length() << std::endl;
        std::cout << "a.Normalized() = " << a.Normalized() << std::endl;
        std::cout << "a.Dot(b) = " << a.Dot(b) << std::endl;
        std::cout << "a.Distance(b) = " << a.Distance(b) << std::endl;
        std::cout << "a.Rotated(90°) = " << a.Rotated(LikesProgram::Math::PI / 2) << std::endl;
        std::cout << "a.Perpendicular() = " << a.Perpendicular() << std::endl;
        std::cout << "a.Clamped(2.0) = " << a.Clamped(2.0) << std::endl;
        std::cout << "a.Cross(b) = " << a.Cross(b) << std::endl;
        std::cout << "a.Angle() = " << a.Angle() << std::endl;
        std::cout << "a.AngleBetween(b) = " << a.AngleBetween(b) << std::endl;
        std::cout << "a.Reflected(b) = " << a.Reflected(b) << std::endl;
        std::cout << "a.Project(b) = " << a.Project(b) << std::endl;
        std::cout << "a.Reject(b) = " << a.Reject(b) << std::endl;
    }

    void LerpTest() {
        std::cout << std::endl << "===== 插值测试 =====" << std::endl;
        LikesProgram::Math::Vector a(0.0, 0.0);
        LikesProgram::Math::Vector b(10.0, 10.0);

        for (double t = 0.0; t <= 1.0; t += 0.25) {
            std::cout << "Lerp(a, b, " << t << ") = " << LikesProgram::Math::Vector::Lerp(a, b, t) << std::endl;
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
