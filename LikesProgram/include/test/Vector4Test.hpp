#pragma once
#include <iostream>
#include <random>
#include "../../include/LikesProgram/math/Math.hpp"
#include "../../include/LikesProgram/math/Vector4.hpp"
#include "../../include/LikesProgram/math/Vector3.hpp"
#include "../../include/LikesProgram/time/Timer.hpp"

namespace Vector4Test {

    void StressTest(size_t count = 100000) {
        std::cout << "\n===== Vector4 Stress Test (" << count << " 次) =====\n";

        std::mt19937 rng(std::random_device{}());
        std::uniform_real_distribution<double> dist(-1000.0, 1000.0);

        LikesProgram::Time::Timer timer(true);

        LikesProgram::Math::Vector4 acc(0.0, 0.0, 0.0, 0.0);
        for (size_t i = 0; i < count; i++) {
            LikesProgram::Math::Vector4 a(dist(rng), dist(rng), dist(rng), dist(rng));
            LikesProgram::Math::Vector4 b(dist(rng), dist(rng), dist(rng), dist(rng));

            acc += a + b;
            acc -= a - b;
            acc *= 0.5;
            acc += a * b; // 分量乘
            acc -= a / (b + LikesProgram::Math::Vector4(1.0, 1.0, 1.0, 1.0));
            acc += LikesProgram::Math::Vector4::Lerp(a, b, 0.5);
        }

        std::cout << "Stress Test 完成\n";
        std::cout << "最终累积结果: " << acc << "\n";
        std::cout << "耗时: " << LikesProgram::Time::NsToMs(timer.Stop().count()) << "ms" << std::endl;
    }

    void BasicOps() {
        std::cout << "===== Vector4 基本运算符测试 =====\n";
        LikesProgram::Math::Vector4 a(1.0, 2.0, 3.0, 4.0);
        LikesProgram::Math::Vector4 b(4.0, 3.0, 2.0, 1.0);

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
        std::cout << "\n===== Vector4 复合赋值测试 =====\n";
        LikesProgram::Math::Vector4 v(1.0, 2.0, 3.0, 4.0);

        std::cout << "v = " << v << "\n";
        v += LikesProgram::Math::Vector4(1.0, 1.0, 1.0, 1.0);
        std::cout << "v += (1,1,1,1) -> " << v << "\n";
        v -= LikesProgram::Math::Vector4(0.5, 0.5, 0.5, 0.5);
        std::cout << "v -= (0.5,0.5,0.5,0.5) -> " << v << "\n";
        v *= 2.0;
        std::cout << "v *= 2 -> " << v << "\n";
        v /= 2.0;
        std::cout << "v /= 2 -> " << v << "\n";
    }

    void MathOps() {
        std::cout << "\n===== Vector4 数学运算测试 =====\n";
        LikesProgram::Math::Vector4 a(1.0, 2.0, 3.0, 4.0);
        LikesProgram::Math::Vector4 b(4.0, 3.0, 2.0, 1.0);

        std::cout << "a = " << a << ", b = " << b << "\n";
        std::cout << "Length(a) = " << a.Length() << "\n";
        std::cout << "a.Normalized() = " << a.Normalized() << "\n";
        std::cout << "a.SafeNormalized() = " << a.SafeNormalized() << "\n";
        std::cout << "a.Dot(b) = " << a.Dot(b) << "\n";
        std::cout << "a.NearlyEquals(b) = " << a.NearlyEquals(b) << "\n";
        std::cout << "a.IsZero() = " << a.IsZero() << "\n";
    }

    void ConversionTest() {
        std::cout << "\n===== Vector4 与 Vector3 转换测试 =====\n";
        LikesProgram::Math::Vector4 v4(1.0, 2.0, 3.0, 4.0);
        LikesProgram::Math::Vector3 v3 = LikesProgram::Math::ToVector3(v4);
        std::cout << "Vector4: " << v4 << " -> Vector3: " << v3 << "\n";
    }

    void InterpolationTest() {
        std::cout << "\n===== Vector4 插值测试 =====\n";
        LikesProgram::Math::Vector4 a(0.0, 0.0, 0.0, 0.0);
        LikesProgram::Math::Vector4 b(1.0, 1.0, 1.0, 1.0);

        for (double t = 0.0; t <= 1.0; t += 0.25) {
            std::cout << "Lerp(a,b," << t << ") = " << LikesProgram::Math::Vector4::Lerp(a, b, t) << "\n";
        }
    }

    void Test() {
        BasicOps();
        CompoundOps();
        MathOps();
        ConversionTest();
        InterpolationTest();
        StressTest();
    }

}
