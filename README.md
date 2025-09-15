# LikesProgram

## 介绍
`LikesProgram` 是一个 **C++ 通用工具库**，提供丰富、高效且易用的类库和工具函数，帮助开发者在各种项目中减少重复代码、提升开发效率，并保证性能和可扩展性。
本库面向多种场景，包括性能分析、数学计算、日志管理、多线程任务调度、国际化字符串处理等，设计时注重易用性和可扩展性。

## 功能概览
```
LikesProgram
├─ Math（数学工具）
│  ├─ 常量：PI、EPSILON、INF
│  ├─ 函数：UpdateMax、EMA、NsToMs、NsToS、MsToNs、SToNs
│  ├─ Vector（二位向量）
│  ├─ Vector3（三维向量）
│  └─ Vector4（四维向量）
├─ Timer（高精度计时器）
│  ├─ 单线程计时
│  ├─ 多线程计时
│  └─ 时间转换与字符串输出
├─ Unicode（Unicode 工具集）
│  ├─ Case（大小写转换）
│  │   ├─ BMP 字符大/小写映射
│  │   └─ SMP 字符大/小写映射
│  └─ Convert（编码转换）
│      ├─ UTF-8 ⇄ UTF-16 ⇄ UTF-32
│      └─ UTF-16 ⇄ GBK
├─ String（Unicode 字符串类）
│  ├─ UTF-8 / UTF-16 / UTF-32 支持
│  └─ 编码转换工具
├─ Logger（日志系统）
│  ├─ 多级别日志
│  └─ 多种输出方式
├─ CoreUtils（系统与辅助工具）
│  ├─ 获取本机 MAC 地址
│  ├─ 获取本机 IP 地址
│  └─ 生成 UUID
├─ ThreadPool（线程池管理）
└─ Config（配置管理）
   ├─ 支持继承 Serializer 自定义序列化格式
   └─ 默认 JSON 支持
```

## 使用须知

LikesProgram 开源并允许自由使用和二次开发，但请注意：

1. 使用本库的二次开发或派生项目 **必须在文档或显著位置注明**：
    > “本项目使用了 LikesProgram 库（作者：龙兵寅，官网：[likesprogram.com](https://likesprogram.com)，Github：[github.com/likesprogram/LikesProgram](https://github.com/likesprogram/LikesProgram)”

2. 禁止用于违法、侵权或伤害他人的行为。
3. 本库不保证适用性、稳定性或安全性，使用者自行承担风险。
4. 本库遵循 **BSD 3-Clause License**。允许自由使用、复制、修改和分发，但必须保留上述署名声明。

## 许可证

本库采用 **BSD 3-Clause License**，详见 LICENSE 文件。

## 功能详解

### 1、Math：数学工具

#### 1.1、常用工具

##### 1.1.1、常量定义

* `PI`：圆周率常量，精度高达 30 位。
* `EPSILON`：浮点数计算精度阈值，用于比较近似相等。
* `INF`：表示**无穷大**（Infinity），常用于初始化最大值、算法的“哨兵值”或需要表示“无限大”的场景。

##### 1.1.2、常用函数

* `UpdateMax(std::atomic<T1>& target, T2 value)`
  更新 `target` 的值为 `value`，仅在 `value > target` 时更新，线程安全。
  用途：在多线程环境下统计最大值。

* `EMA(T1 previous, T2 value, double alpha = 0.9)`
  计算指数移动平均值（Exponential Moving Average），可用于性能监控或平滑数据。

  * `previous`：上一次值
  * `value`：当前值
  * `alpha`：平滑系数（0-1），默认 0.9

* 时间转换函数：方便在纳秒、毫秒、秒之间互相转换

  * `NsToMs(long long ns)`：纳秒 → 毫秒
  * `NsToS(long long ns)`：纳秒 → 秒
  * `MsToNs(long long ms)`：毫秒 → 纳秒
  * `MsToS(long long ms)`：毫秒 → 秒
  * `SToNs(long long s)`：秒 → 纳秒
  * `SToMs(long long s)`：秒 → 毫秒

##### 使用示例

```cpp
#include <iostream>
#include <atomic>
#include <LikesProgram/math/Math.hpp>

int main() {
    std::atomic<int> maxVal = 0;
    LikesProgram::Math::UpdateMax(maxVal, 10);
    std::cout << "最大值: " << maxVal << std::endl;

    double ema = LikesProgram::Math::EMA(10, 20, 0.8);
    std::cout << "EMA: " << ema << std::endl;

    long long ns = 1'500'000'000;
    std::cout << "纳秒转秒: " << LikesProgram::Math::NsToS(ns) << " s" << std::endl;
}
```

### 1.2、Vector：二维向量类

`Vector` 类提供二维向量运算的支持，适用于几何计算、物理模拟和游戏开发等场景。

#### 构造函数

* `Vector()`：默认构造，初始化为 `(0, 0)`
* `Vector(double x, double y)`：指定 x、y 坐标
* `explicit Vector(double s)`：同时初始化 x 和 y 为同一标量 `s`

#### 运算符重载

* **标量运算**：

  * `v + s` / `v - s` / `v * s` / `v / s`
  * `v += s` / `v -= s` / `v *= s` / `v /= s`

* **向量运算**：

  * `v + u` / `v - u` / `v * u` (分量乘) / `v / u` (分量除)
  * `v += u` / `v -= u` / `v *= u` / `v /= u`

* **一元运算**：

  * `-v`：取负向量
  * `+v`：取正向量

* **自增自减**：

  * `++v` / `v++` / `--v` / `v--`

* **比较运算**：

  * `==` / `!=` / `<` / `>` / `<=` / `>=`
  * `<` 使用字典序比较

* **索引访问**：

  * `v[0]` → x
  * `v[1]` → y

* **友元运算符**：

  * `operator*(double s, const Vector& v)`：标量左乘
  * `operator<<(std::ostream& os, const Vector& v)`：输出格式 `(x, y)`

#### 常用方法

* `Length()`：返回向量长度
* `Normalized()`：返回单位向量
* `Dot(const Vector& v)`：点积
* `Distance(const Vector& v)`：两向量距离
* `Rotated(double angle)`：逆时针旋转 `angle` 弧度
* `IsZero()`：判断是否为零向量
* `Perpendicular()`：返回垂直向量
* `Clamped(double maxLength)`：限制向量最大长度
* `Cross(const Vector& v)`：二维叉积，返回标量
* `Angle()`：返回极角 `atan2(y, x)`
* `AngleBetween(const Vector& v)`：返回夹角 `[0, pi]`
* `Reflected(const Vector& normal)`：沿单位法线反射
* `Project(const Vector& on)`：向量投影
* `Reject(const Vector& on)`：向量拒投影（去掉投影部分）
* `static Lerp(const Vector& a, const Vector& b, double t)`：线性插值

#### 使用示例

```cpp
#include <iostream>
#include <random>
#include <LikesProgram/math/Vector.hpp>
#include <LikesProgram/math/Math.hpp>
#include <LikesProgram/Timer.hpp>

namespace VectorTest {
    void StressTest(size_t count = 100000) {
        std::cout << std::endl << "===== 随机 Stress Test (" << count << " 次) =====" << std::endl;

        std::mt19937 rng(std::random_device{}());
        std::uniform_real_distribution<double> dist(-1000.0, 1000.0);

        LikesProgram::Timer::Start(); // 开始计时

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
        std::cout << "耗时: " << LikesProgram::Timer::ToString(LikesProgram::Timer::Stop()) << std::endl;
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
```

#### 1.3、Vector3：三维向量类 (未完成)
#### 1.4、Vector4：4维向量类 (未完成)

### 2、Timer：高精度计时器

`Timer` 提供单线程与多线程的高精度计时功能，方便性能分析、调试及任务耗时统计。

#### 构造函数

* `Timer(bool autoStart = false)`：构造计时器，可选择是否自动启动。

#### 静态方法（线程局部）

* `Start()`：开始计时
* `Stop(double alpha = 0.9)`：停止计时，并返回本次耗时。`alpha` 用于 EMA 平均计算
* `ResetThread()`：重置当前线程的计时数据
* `GetLastElapsed()`：获取最近一次 Stop() 的耗时
* `GetTotalElapsed()`：获取线程内累计耗时
* `GetEMAAverageElapsed()`：获取线程内指数移动平均耗时
* `GetArithmeticAverageElapsed()`：获取线程内算术平均耗时
* `IsRunning()`：当前线程是否在计时

#### 静态方法（全局 / 多线程共享）

* `ResetGlobal()`：重置全局计时数据
* `GetTotalGlobalElapsed()`：获取全局累计耗时
* `GetLongestElapsed()`：获取历史最长耗时
* `GetArithmeticAverageGlobalElapsed()`：获取全局算术平均耗时

#### 辅助方法

* `ToString(Duration duration)`：将时间间隔转换为可读字符串
* `NowNs()`（私有）：获取高精度纳秒时间

#### 使用示例

```cpp
#include <iostream>
#include <thread>
#include <vector>
#include <LikesProgram/Timer.hpp>

namespace TimerTest {
	void WorkLoad(size_t id) {
		LikesProgram::Timer::Start(); // 开始计时

		// 模拟耗时操作
		std::this_thread::sleep_for(std::chrono::milliseconds(100 + id * 250));

		auto elapsed = LikesProgram::Timer::Stop(); // 停止并获取耗时
        std::cout << "Thread 【" << id << "】：" << LikesProgram::Timer::ToString(elapsed) << std::endl;
	}

    void Test() {
		std::cout << "===== 单线程示例 =====" << std::endl;
		{
            LikesProgram::Timer timer(true); // 构造并启动
            std::this_thread::sleep_for(std::chrono::milliseconds(250));

			std::cout << "是否运行：" << LikesProgram::Timer::IsRunning() << std::endl;
			auto elapsed = LikesProgram::Timer::Stop();
            std::cout << "单线程：" << LikesProgram::Timer::ToString(elapsed) << std::endl;

			std::cout << "是否运行：" << LikesProgram::Timer::IsRunning() << std::endl;
			std::cout << "最近一次耗时：" << LikesProgram::Timer::ToString(LikesProgram::Timer::GetLastElapsed()) << std::endl;
		}

		std::cout << std::endl << "===== 多线程示例 =====" << std::endl;
		{
			const int threadCount = 4;
            std::vector<std::thread> threads;

			// 创建多个线程
			for (size_t i = 0; i < threadCount; i++) {
				threads.emplace_back(WorkLoad, i);
			}

			for (auto& thread : threads) thread.join();

			std::cout << std::endl << "===== 测试结果 =====" << std::endl;
            std::cout << "线程总时间：" << LikesProgram::Timer::ToString(LikesProgram::Timer::GetTotalElapsed()) << std::endl;
			std::cout << "总时间：" << LikesProgram::Timer::ToString(LikesProgram::Timer::GetTotalGlobalElapsed()) << std::endl;
            std::cout << "最长时间：" << LikesProgram::Timer::ToString(LikesProgram::Timer::GetLongestElapsed()) << std::endl;
            std::cout << "EMA平均时间：" << LikesProgram::Timer::ToString(LikesProgram::Timer::GetEMAAverageElapsed()) << std::endl;
			std::cout << "线程算数平均时间：" << LikesProgram::Timer::ToString(LikesProgram::Timer::GetArithmeticAverageElapsed()) << std::endl;
			std::cout << "全局算数平均时间：" << LikesProgram::Timer::ToString(LikesProgram::Timer::GetArithmeticAverageGlobalElapsed()) << std::endl;
		}

		std::cout << std::endl << "===== 重置示例 =====" << std::endl;
		{
			LikesProgram::Timer::ResetThread(); // 重置当前线程计时数据
			LikesProgram::Timer::ResetGlobal(); // 重置全局计时数据

			std::cout << "线程总时间：" << LikesProgram::Timer::ToString(LikesProgram::Timer::GetTotalElapsed()) << std::endl;
			std::cout << "总时间：" << LikesProgram::Timer::ToString(LikesProgram::Timer::GetTotalGlobalElapsed()) << std::endl;
            std::cout << "最长时间：" << LikesProgram::Timer::ToString(LikesProgram::Timer::GetLongestElapsed()) << std::endl;
			std::cout << "EMA平均时间：" << LikesProgram::Timer::ToString(LikesProgram::Timer::GetEMAAverageElapsed()) << std::endl;
			std::cout << "线程算数平均时间：" << LikesProgram::Timer::ToString(LikesProgram::Timer::GetArithmeticAverageElapsed()) << std::endl;
			std::cout << "全局算数平均时间：" << LikesProgram::Timer::ToString(LikesProgram::Timer::GetArithmeticAverageGlobalElapsed()) << std::endl;
		}
    }
}
```

### 3、Unicode：Unicode 工具集

#### 3.1、Case：大小写转换
##### 3.1.1、BMP 大小写映射
* `BMPToUpper(uint16_t c)` / `BMPToLower(uint16_t c)`
  支持 Basic Multilingual Plane 范围内字符（拉丁字母、希腊字母、俄文等）。

##### 3.1.2、SMP 大小写映射
* `SMPToUpper(uint32_t c)` / `SMPToLower(uint32_t c)`
  覆盖 Supplementary Multilingual Plane（如古文字、数学字母等）。

#### 3.2、Convert：编码转换

##### 3.2.1、UTF-8 ⇄ UTF-16
* `Utf8ToUtf16(const std::u8string& utf8)`
  UTF-8 转 UTF-16
* `Utf16ToUtf8(const std::u16string& utf16)`
  UTF-16 转 UTF-8

##### 3.2.2、UTF-32 ⇄ UTF-16
* `Utf32ToUtf16(const std::u32string& utf32)`
  UTF-32 转 UTF-16
* `Utf16ToUtf32(const std::u16string& utf16)`
  UTF-16 转 UTF-32

##### 3.2.3、GBK ⇄ UTF-16
* `GbkToUtf16(const std::string& gbk)`
  GBK 转 UTF-16
* `Utf16ToGbk(const std::u16string& utf16)`
  UTF-16 转 GBK

#### 3.3、使用示例
```cpp
#pragma once
#include <iostream>
#include <iomanip>
#include "../LikesProgram/unicode/Unicode.hpp"

namespace UnicodeTest {
    void TestBMP_AllLatin() {
        std::cout << "\n=== BMP Latin Full ===\n";

        for (char16_t c = u'a'; c <= u'z'; ++c) {
            uint32_t upper = LikesProgram::Unicode::Case::BMPToUpper(c);

            std::cout << (char)c << " -> " << (char)LikesProgram::Unicode::Case::BMPToUpper(c)
            << " : U+" << std::hex << std::uppercase << static_cast<uint32_t>(c)
            << " -> U+" << static_cast<uint32_t>(upper) << "\n";
        }
        for (char16_t c = u'A'; c <= u'Z'; ++c) {
            uint32_t lower = LikesProgram::Unicode::Case::BMPToLower(c);

            std::cout << (char)c << " -> " << (char)LikesProgram::Unicode::Case::BMPToLower(c)
            << " : U+" << std::hex << std::uppercase << static_cast<uint32_t>(c)
            << " -> U+" << static_cast<uint32_t>(lower) << "\n";
        }
    }

    void TestBMP_Extended() {
        std::cout << "\n=== BMP Extended ===\n";
        char16_t samples[] = {
            0x00E1, 0x00C0, 0x00FC, 0x00DF, // á À ü ß
            0x0100, 0x0101, 0x0130, 0x0131, // Ā ā İ ı
            0x03B1, 0x03C9, 0x0391, 0x03A9, // α ω Α Ω
            0x0410, 0x0430                // А а
        };

        for (auto c : samples) {
            uint32_t u = LikesProgram::Unicode::Case::BMPToUpper(c);
            uint32_t l = LikesProgram::Unicode::Case::BMPToLower(c);
            std::cout << "U+"
                << std::hex << std::uppercase << static_cast<uint32_t>(c)
                << " upper->U+" << u
                << " lower->U+" << l
                << "\n";
        }
    }

    void TestBMP() {
        TestBMP_AllLatin();
        TestBMP_Extended();
    }

    void TestSMP() {
        std::cout << "\n=== SMP Tests ===\n";

        uint32_t samples[] = {
            0x10400, 0x10428, 0x104B0, 0x104D8, // Deseret / Osage
            0x118A0, 0x118C0, 0x118BA, 0x118DA, // Warang Citi
            0x16E40, 0x16E60                  // Medefin / Supplementary examples
        };

        for (auto c : samples) {
            uint32_t u = LikesProgram::Unicode::Case::SMPToUpper(c);
            uint32_t l = LikesProgram::Unicode::Case::SMPToLower(c);
            std::cout << "U+"
                << std::hex << std::uppercase << c
                << " -> upper: U+" << u
                << " lower: U+" << l
                << "\n";
        }
    }

    void ValidateSMP() {
        struct TestCase {
            uint32_t input;
            uint32_t expected_upper;
            uint32_t expected_lower;
        };

        TestCase test_cases[] = {
            {0x10400, 0x10400, 0x10428},
            {0x10428, 0x10400, 0x10428},
            {0x104B0, 0x104B0, 0x104D8},
            {0x104D8, 0x104B0, 0x104D8},
            {0x118A0, 0x118A0, 0x118C0},
            {0x118C0, 0x118A0, 0x118C0},
            {0x118BA, 0x118BA, 0x118DA},
            {0x118DA, 0x118BA, 0x118DA},
            {0x16E40, 0x16E40, 0x16E60},
            {0x16E60, 0x16E40, 0x16E60}
        };

        std::cout << "\n=== SMP Validation ===\n";

        for (auto& tc : test_cases) {
            uint32_t u = LikesProgram::Unicode::Case::SMPToUpper(tc.input);
            uint32_t l = LikesProgram::Unicode::Case::SMPToLower(tc.input);

            std::cout << "0x"
                << std::hex << std::uppercase << tc.input
                << " -> upper: 0x" << u
                << " (expected 0x" << tc.expected_upper << ")"
                << " -> lower: 0x" << l
                << " (expected 0x" << tc.expected_lower << ")";

            if (u != tc.expected_upper || l != tc.expected_lower) {
                std::cout << "  [错]";
            }
            else {
                std::cout << "  [对]";
            }
            std::cout << "\n";
        }
    }
    void TestConvert() {
        std::cout << "\n=== Convert ===\n";
        // UTF-8 → UTF-16
        std::u8string utf8 = u8"你好，Unicode！";
        std::u16string u16 = LikesProgram::Unicode::Convert::Utf8ToUtf16(utf8);
        std::cout << "UTF-8 转 UTF-16 长度: " << std::dec << u16.size() << "\n";

        // UTF-32 → UTF-16
        std::u32string u32 = U"𐐷𐑊"; // Deseret letters
        std::u16string u16_from32 = LikesProgram::Unicode::Convert::Utf32ToUtf16(u32);
        std::cout << "UTF-32 转 UTF-16 长度: " << u16_from32.size() << "\n";

        // UTF-16 → UTF-8
        std::u8string backToUtf8 = LikesProgram::Unicode::Convert::Utf16ToUtf8(u16);
        std::cout << "UTF-16 转 UTF-8: " << backToUtf8.size() << "\n";

        // UTF-16 → UTF-32
        std::u32string backToUtf32 = LikesProgram::Unicode::Convert::Utf16ToUtf32(u16_from32);
        std::cout << "UTF-16 转 UTF-32 长度: " << backToUtf32.size() << "\n";

        // UTF-16 ↔ GBK （仅在 Windows 或启用 iconv 的平台可用）
        std::u16string chinese16 = u"汉字";
        std::string gbk = LikesProgram::Unicode::Convert::Utf16ToGbk(chinese16);
        std::u16string backToUtf16 = LikesProgram::Unicode::Convert::GbkToUtf16(gbk);
        std::cout << "GBK 往返长度: " << backToUtf16.size() << "\n";
        std::cout << "GBK: " << gbk << "\n";
    }

    void Test() {
        TestBMP();
        TestSMP();
        ValidateSMP();
        TestConvert();
    }

}
```

### 4、String：Unicode 字符串类

`String` 类提供对多种编码的 Unicode 字符串的封装，内部统一以 UTF-16 存储，支持字符访问、子串操作、大小写转换、查找和分割等功能，适用于文本处理、文件解析和网络通信等场景。

#### 构造函数

* `String()`：默认构造，创建空字符串
* `explicit String(const char* s, Encoding enc = Encoding::UTF8)`：从 C 风格字符串构造，默认按 UTF-8 解析
* `explicit String(const char8_t* s)`：从 UTF-8 字符串构造
* `explicit String(const char16_t* s)`：从 UTF-16 字符串构造
* `explicit String(const char32_t* s)`：从 UTF-32 字符串构造
* `String(const String& other)`：拷贝构造
* `String(String&& other) noexcept`：移动构造
* `explicit String(const char8_t c)`：构造单字符 UTF-8 字符串
* `explicit String(const char16_t c)`：构造单字符 UTF-16 字符串
* `explicit String(const char32_t c)`：构造单字符 UTF-32 字符串
* `explicit String(const std::string& s, Encoding enc = Encoding::UTF8)`：从 std::string 构造
* `explicit String(const std::u8string& s)`：从 std::u8string 构造
* `explicit String(const std::wstring& s)`：从 std::wstring 构造
* `explicit String(const std::u16string& s)`：从 std::u16string 构造
* `explicit String(const std::u32string& s)`：从 std::u32string 构造

#### 赋值操作

* `String& operator=(const String& other)`：拷贝赋值
* `String& operator=(String&& other) noexcept`：移动赋值

#### 字符与长度访问

* `size_t Size() const`：返回字符数量（Unicode aware）
* `size_t Length() const`：同 `Size()`
* `bool Empty() const`：判断字符串是否为空
* `void Clear()`：清空字符串
* `char32_t At(size_t index) const`：安全访问指定索引字符
* `char32_t Front() const`：获取第一个字符
* `char32_t Back() const`：获取最后一个字符

#### 拼接与子串操作

* `String& Append(const String& str)`：追加字符串
* `String& operator+=(const String& str)`：追加字符串
* `String SubString(size_t index, size_t count) const`：获取指定范围子串
* `String Left(size_t count) const`：截取左侧子串
* `String Right(size_t count) const`：截取右侧子串

#### 大小写转换

* `String ToUpper() const`：返回大写字符串
* `String ToLower() const`：返回小写字符串
* `void ToUpperInPlace()`：原地转换为大写
* `void ToLowerInPlace()`：原地转换为小写

#### 查找与匹配

* `size_t Find(const String& str, size_t start = 0) const`：查找子串
* `size_t LastFind(const String& str, size_t start = 0) const`：反向查找子串
* `bool StartsWith(const String& str) const`：判断是否以指定子串开头
* `bool EndsWith(const String& str) const`：判断是否以指定子串结尾
* `bool EqualsIgnoreCase(const String& other) const`：忽略大小写比较

#### 运算符重载

* 比较运算：`==` / `!=` / `<` / `<=` / `>` / `>=`
  `<` 采用字典序比较
* 拼接运算：`operator+=`
* 迭代器支持范围 for 循环：`begin()` / `end()`

#### 转换函数

* `std::string ToStdString(Encoding enc = Encoding::UTF8) const`：转换为 std::string
* `std::wstring ToWString() const`：转换为 std::wstring
* `std::u16string ToU16String() const`：转换为 std::u16string
* `std::u32string ToU32String() const`：转换为 std::u32string

#### 分割操作

* `std::vector<String> Split(const String& sep) const`：按指定分隔符拆分字符串

#### 使用示例

```cpp
#pragma once
#include <iostream>
#include <LikesProgram/String.hpp>

namespace StringTest {
    void Test() {
        // 构造测试
        LikesProgram::String s1(u"Hello 世界");      // UTF-16
        LikesProgram::String s2("hello world");      // UTF-8 默认
        LikesProgram::String s3(U"🌟星");           // UTF-32 emoji + 中文
        LikesProgram::String s4 = s1;                // 拷贝构造
        LikesProgram::String s5 = std::move(s2);     // 移动构造

        std::cout << "s1 size: " << s1.Size() << "\n"; // Unicode code points
        std::cout << "s3 size: " << s3.Size() << "\n";

        // 拼接
        s1.Append(s3);
        std::cout << "After append, s1 size: " << s1.Size() << "\n";

        // 子串
        LikesProgram::String sub = s1.SubString(0, 5);
        std::cout << "SubString(0,5) size: " << sub.Size() << "\n";

        // 大小写转换
        LikesProgram::String upper = s1.ToUpper();
        LikesProgram::String lower = s1.ToLower();
        std::cout << "Upper: " << upper.ToStdString(LikesProgram::String::Encoding::GBK) << "\n";
        std::cout << "Lower: " << lower.ToStdString(LikesProgram::String::Encoding::GBK) << "\n";

        // 查找
        size_t idx = s1.Find(LikesProgram::String(u"世界"));
        std::cout << "Find '世界': " << idx << "\n";

        size_t last_idx = s1.LastFind(LikesProgram::String(u"星"));
        std::cout << "LastFind '星': " << last_idx << "\n";

        // StartsWith / EndsWith
        std::cout << "StartsWith 'Hello': " << s1.StartsWith(LikesProgram::String(u"Hello")) << "\n";
        std::cout << "EndsWith '星': " << s1.EndsWith(LikesProgram::String(U"星")) << "\n";

        // 忽略大小写比较
        LikesProgram::String cmp1("Test");
        LikesProgram::String cmp2("tEsT");
        std::cout << "EqualsIgnoreCase: " << cmp1.EqualsIgnoreCase(cmp2) << "\n";

        // 迭代器
        std::cout << "Iterate code points: ";
        for (auto cp : s1) {
            std::cout << std::hex << "U+" << static_cast<uint32_t>(cp) << " ";
        }
        std::cout << "\n";

        // 分割
        LikesProgram::String s6("a,b,c,d");
        auto parts = s6.Split(LikesProgram::String(u","));
        std::cout << "Split: ";
        for (auto& p : parts) {
            std::cout << p.ToStdString() << " ";
        }
        std::cout << "\n";
    }
}
```

### 5、Logger：灵活的日志系统 (未完成)
### 6、ThreadPool：线程池 (未完成)
### 7、CoreUtils：辅助工具类 (未完成)
