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
│  ├─ 获取设置与获取线程名称
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

## 使用说明

### 编译

1. 确保已安装 [CMake](https://cmake.org/)（≥ 3.15），以及 C++20 语言支持

2. 在项目根目录下新建一个 `build` 目录并进入：

   ```bash
   mkdir build
   cd build
   ```

3. 运行 CMake 配置命令：

   ```bash
   cmake ..
   ```

   常用选项：

   * `-DBUILD_SHARED_LIBS=[ON|OFF]`
     是否生成动态库（ON）或静态库（OFF），默认：ON（动态库）。
   * `-DENABLE_EXAMPLES=[ON|OFF]`
     是否构建示例程序 `LikesProgramDemo`，默认：OFF。
   * `-DENABLE_STRICT_WARNINGS=[ON|OFF]`
     是否开启编译器严格警告，默认：ON。

4. 如需修改安装路径，可编辑 `build/cmake_install.cmake` 文件，将 `CMAKE_INSTALL_PREFIX` 设置为目标安装目录。

5. 编译并安装：

   ```bash
   cmake --build . --config Release --target INSTALL
   ```

6. 安装完成后，在 `CMAKE_INSTALL_PREFIX` 目录下会看到：

   ```
   include/   # 头文件
   lib/       # 库文件 (LikesProgram.lib / LikesProgram.dll 或 .a / .so)
   ```

### 使用方法

1. 在项目中包含头文件：

   ```cpp
   #include <LikesProgram/String.hpp>
   #include <LikesProgram/Timer.hpp>
   // ...
   ```
2. 链接库：

   * Windows (MSVC)：

     ```cmake
     target_link_libraries(your_target PRIVATE LikesProgram)
     ```
   * Linux/macOS：

     ```bash
     g++ main.cpp -I/path/to/include -L/path/to/lib -lLikesProgram
     ```

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

### 2、`Timer` — 高精度计时器

`Timer` 用于测量代码片段的耗时，支持单线程与多线程环境，精度可达纳秒级。它既能作为轻量的局部计时器，也可以在父对象中聚合多线程的统计信息，方便性能分析、调试和任务耗时监控。

#### 构造与生命周期

* `Timer(bool autoStart = false, Timer* parent = nullptr)`
  创建计时器，可选是否立即启动，并可指定父 `Timer` 用于汇总统计。

#### 基本操作

* `Start()` 开始计时。
* `Stop(double alpha = 0.9)` 停止计时并返回本次耗时，同时更新累计时间、最长耗时和平均值（`alpha` 控制 EMA 平滑系数）。
* `Reset()` 重置计时器状态。
* `ResetThread()` 仅清除当前线程的数据。
* `ResetGlobal()` 清除全局统计信息。

#### 查询接口

* `GetLastElapsed() const` 上一次 `Stop()` 的耗时。
* `GetTotalElapsed() const` 累计耗时。
* `GetLongestElapsed() const` 记录到的最长耗时。
* `GetEMAAverageElapsed() const` 指数移动平均耗时。
* `GetArithmeticAverageElapsed() const` 算术平均耗时。
* `IsRunning() const` 当前是否正在计时。

#### 工具方法

* `static String ToString(Duration d)` 将时间间隔格式化为易读字符串。

#### 使用示例

```cpp
#include <iostream>
#include <thread>
#include <vector>
#include <LikesProgram/Timer.hpp>

namespace TimerTest {
	struct ThreadData {
        LikesProgram::Timer* timer = nullptr;
		size_t index = 0;
	};

	void WorkLoad(ThreadData* data) {
		LikesProgram::Timer threadTimer(true, data->timer); // 创建线程计时器

		// 模拟耗时操作
		std::this_thread::sleep_for(std::chrono::milliseconds(100 + data->index * 20));

		auto elapsed = threadTimer.Stop(); // 停止并获取耗时
        std::cout << "Thread 【" << data->index << "】：" << LikesProgram::Timer::ToString(elapsed) << std::endl;
	}

    void Test() {
		LikesProgram::Timer timer; // 全局计时器

		std::cout << "===== 单线程示例 =====" << std::endl;
		{
			timer.Start(); // 开始计时
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

			//std::cout << "是否运行：" << timer.IsRunning() << std::endl;
			auto elapsed = timer.Stop();
            std::cout << "单线程：" << LikesProgram::Timer::ToString(elapsed) << std::endl;

			std::cout << "是否运行：" << timer.IsRunning() << std::endl;
			std::cout << "最近一次耗时：" << LikesProgram::Timer::ToString(timer.GetLastElapsed()) << std::endl;
		}

		std::cout << std::endl << "===== 多线程示例 =====" << std::endl;
		{
			const int threadCount = 4;
            std::vector<std::thread> threads;

			// 创建多个线程
			for (size_t i = 0; i < threadCount; i++) {
				ThreadData* data = new ThreadData();
				data->index = i;
                data->timer = &timer;
				threads.emplace_back(WorkLoad, data);
			}

			for (auto& thread : threads) thread.join();
			std::cout << std::endl << "===== 测试结果 =====" << std::endl;
			std::cout << "线程总时间：" << LikesProgram::Timer::ToString(timer.GetTotalElapsed()) << std::endl;
			std::cout << "最长时间：" << LikesProgram::Timer::ToString(timer.GetLongestElapsed()) << std::endl;
			std::cout << "EMA平均时间：" << LikesProgram::Timer::ToString(timer.GetEMAAverageElapsed()) << std::endl;
			std::cout << "线程算数平均时间：" << LikesProgram::Timer::ToString(timer.GetArithmeticAverageElapsed()) << std::endl;
		}

		std::cout << std::endl << "===== 重置示例 =====" << std::endl;
		{
			timer.Reset();

			std::cout << "线程总时间：" << LikesProgram::Timer::ToString(timer.GetTotalElapsed()) << std::endl;
			std::cout << "最长时间：" << LikesProgram::Timer::ToString(timer.GetLongestElapsed()) << std::endl;
			std::cout << "EMA平均时间：" << LikesProgram::Timer::ToString(timer.GetEMAAverageElapsed()) << std::endl;
			std::cout << "线程算数平均时间：" << LikesProgram::Timer::ToString(timer.GetArithmeticAverageElapsed()) << std::endl;
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
        std::cout << "Utf16: ";
        for (char16_t c : backToUtf16) {
            std::cout << std::hex << std::showbase << (uint16_t)c << ' ';
        }
        std::cout << '\n';
        std::cout << "GBK: ";
        for (unsigned char c : gbk) {
            std::cout << std::hex << std::showbase << (int)c << ' ';
        }
        std::cout << std::dec << std::noshowbase << '\n';
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
* `String(const char8_t* s)`：从 UTF-8 字符串构造
* `String(const char16_t* s)`：从 UTF-16 字符串构造
* `String(const char32_t* s)`：从 UTF-32 字符串构造
* `String(const String& other)`：拷贝构造
* `String(String&& other) noexcept`：移动构造
* `String(const char8_t c)`：构造单字符 UTF-8 字符串
* `String(const char16_t c)`：构造单字符 UTF-16 字符串
* `String(const size_t count, const char16_t c)`：构造指定数量的相同字符的字符串
* `String(const char32_t c)`：构造单字符 UTF-32 字符串
* `explicit String(const std::string& s, Encoding enc = Encoding::UTF8)`：从 std::string 构造
* `explicit String(const std::u8string& s)`：从 std::u8string 构造
* `explicit String(const std::wstring& s)`：从 std::wstring 构造
* `explicit String(const std::u16string& s)`：从 std::u16string 构造
* `explicit String(const std::u32string& s)`：从 std::u32string 构造

#### 输入与输出

* `std::ostream& operator<<(std::ostream& os, const String& str)`：输出
* `std::istream& operator>>(std::istream& is, String& str)`：输入
* `std::wostream& operator<<(std::wostream& os, const String& str)`：输出
* `std::wistream& operator>>(std::wistream& is, String& str)`：输入

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
    // 输入输出测试
    void OutAndIn() {
        std::cout << "===== 输出输出示例 =====" << std::endl;
#ifdef _WIN32
        LikesProgram::String str1("", LikesProgram::String::Encoding::GBK); // Windows 控制台输入需要先设置编码为 GBK，Linux 控制台输入可使用默认 UTF-8
#else
        LikesProgram::String str1; // Linux 控制台输入可使用默认 UTF-8
#endif
        std::cout << "请输入字符串[cin]：";
        std::cin >> str1;
        std::cout << "[cout]：" << str1 << "\n";

        LikesProgram::String str2;
        std::cout << "请输入字符串[wcin]：";
        std::wcin >> str2;
        std::wcout << "[wcout]：" << str2 << "\n";
    }

    void Test() {
        OutAndIn();
        std::cout << "===== 其他示例 =====" << std::endl;
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
#ifdef _WIN32
        std::cout << "upper: " << upper.ToStdString(LikesProgram::String::Encoding::GBK) << "\n";
        std::cout << "lower: " << lower.ToStdString(LikesProgram::String::Encoding::GBK) << "\n";
#else
        std::cout << "upper: " << upper.ToStdString() << "\n";
        std::cout << "lower: " << lower.ToStdString() << "\n";
#endif

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

### 5、CoreUtils：辅助工具类

#### 5.1、线程工具
* `SetCurrentThreadName(const LikesProgram::String& name)`
  将当前线程命名。
  用途：调试和日志区分线程。

* `LikesProgram::String GetCurrentThreadName()`
  获取当前线程的名称。
  用途：调试和日志区分线程。

#### 5.2、网络工具

* `LikesProgram::String GetMACAddress()`
  返回本机网络接口的 MAC 地址。
  用途：唯一标识机器，网络注册或日志追踪。

* `LikesProgram::String GetLocalIPAddress()`
  返回本机 IPv4 地址。
  用途：本机网络通信或日志记录。

#### 5.3、UUID 工具

* `LikesProgram::String GenerateUUID(LikesProgram::String prefix = u"")`
  生成全局唯一标识符（UUID），可加可选前缀。
  用途：唯一标识对象、会话或数据记录。

#### 5.4、示例

```cpp
#include <iostream>
#include <LikesProgram/CoreUtils.hpp>

int main() {
    // 设置线程名
    LikesProgram::CoreUtils::SetCurrentThreadName(u"MainThread");
    std::wcout << L"线程名: " << LikesProgram::CoreUtils::GetCurrentThreadName().ToWString() << std::endl;

    // 获取 MAC 和 IP
    std::wcout << L"MAC: " << LikesProgram::CoreUtils::GetMACAddress().ToWString() << std::endl;
    std::wcout << L"IP: " << LikesProgram::CoreUtils::GetLocalIPAddress().ToWString() << std::endl;

    // 生成 UUID
    auto uuid = LikesProgram::CoreUtils::GenerateUUID(u"user_");
    std::wcout << L"UUID: " << uuid.ToWString() << std::endl;
}
```

### 6、Logger：灵活的日志系统

`Logger` 提供了线程安全的日志记录功能，支持多种日志级别、可扩展的日志输出目标（Sink），并可在后台异步处理日志。它采用单例模式，保证全局唯一实例。

#### 日志级别

* `Trace` 最详细的日志，用于跟踪程序细粒度行为
* `Debug` 开发阶段有用的信息
* `Info` 程序运行时信息，表示正常状态
* `Warn` 警告信息，可能存在潜在问题
* `Error` 错误信息，某些功能可能失效
* `Fatal` 致命错误，程序无法继续运行

#### 核心结构

* `Logger::LogMessage`
  代表一条日志，包括以下信息：

  * `level` 日志级别
  * `msg` 日志内容
  * `file` 源文件名
  * `line` 代码行号
  * `func` 函数名
  * `tid` 线程 ID
  * `threadName` 线程名称
  * `timestamp` 日志时间

* `Logger::ILogSink`
  日志输出接口（抽象类），子类实现 `Write()` 方法完成日志写入。
  提供辅助函数：

  * `FormatLogMessage()` 格式化日志内容
  * `LevelToString()` 将日志级别转字符串

#### 获取与配置

* `Logger& Logger::Instance()`
  获取全局唯一实例（单例）。

* `void SetLevel(LogLevel level)`
  设置日志最低级别，低于该级别的日志将被过滤。

* `void SetEncoding(String::Encoding encoding)`
  设置日志输出编码（UTF-8/UTF-16 等）。

* `void AddSink(std::shared_ptr<ILogSink> sink)`
  添加一个日志输出目标，可添加多个 Sink。

#### 记录日志

* `void Log(LogLevel level, const String& msg, const char* file, int line, const char* func)`
  记录一条日志。
  推荐使用宏接口快速记录：

  ```cpp
  LOG_TRACE(msg);
  LOG_DEBUG(msg);
  LOG_INFO(msg);
  LOG_WARN(msg);
  LOG_ERROR(msg);
  LOG_FATAL(msg);
  ```

#### 管理与生命周期

* `void Shutdown()`
  停止日志系统，结束后台线程并清理资源。

#### 工厂函数

* `std::shared_ptr<Logger::ILogSink> CreateConsoleSink()`
  创建控制台输出 Sink。

* `std::shared_ptr<Logger::ILogSink> CreateFileSink(const String& filename)`
  创建文件输出 Sink，写入指定文件。

#### 使用示例

```cpp
#include <LikesProgram/Logger.hpp>
#include <LikesProgram/String.hpp>

namespace LoggerTest {
    // 自定义网络输出 Sink
    class NetworkSink : public LikesProgram::Logger::ILogSink {
    public:
        NetworkSink(const std::string& serverAddress) : server(serverAddress) {}

        void Write(const LikesProgram::Logger::LogMessage& message,
            LikesProgram::Logger::LogLevel minLevel,
            LikesProgram::String::Encoding encoding) override {
            LikesProgram::String formatted = FormatLogMessage(message, minLevel);
            SendToServer(formatted.ToStdString(encoding));
        }

    private:
        std::string server;

        void SendToServer(const std::string& payload) {
            // 这里可以实现 HTTP/TCP 发送逻辑
            // 示例中仅打印到控制台表示发送
            std::cout << "[SEND TO SERVER " << server << "] " << payload << std::endl;
        }
    };

    // 工厂函数
    std::shared_ptr<LikesProgram::Logger::ILogSink> CreateNetworkSink(const std::string& serverAddress) {
        return std::make_shared<NetworkSink>(serverAddress);
    }

	void Test() {
        // 初始化日志
        auto& logger = LikesProgram::Logger::Instance();
#ifdef _WIN32
        logger.SetEncoding(LikesProgram::String::Encoding::GBK);
#endif
#ifdef _DEBUG
        logger.SetLevel(LikesProgram::Logger::LogLevel::Debug);
#else
        logger.SetLevel(LogLevel::Info);
#endif
        // 内置控制台输出 Sink
        logger.AddSink(LikesProgram::Logger::CreateConsoleSink()); // 输出到控制台
        // 内置输出到文件 Sink
        logger.AddSink(LikesProgram::Logger::CreateFileSink(u"app.log")); // 输出到文件
        // 自定义网络输出 Sink
        logger.AddSink(CreateNetworkSink("127.0.0.1:9000")); // 自定义输出 Sink
#ifdef _DEBUG
        LOG_TRACE(u"trace message 日志输出");   // 不会输出
        LOG_DEBUG(u"debug message 日志输出");   // 会输出
        LOG_INFO(u"info message 日志输出");     // 会输出
        LOG_WARN(u"warn message 日志输出");     // 会输出
        LOG_ERROR(u"error message 日志输出");   // 会输出
        LOG_FATAL(u"fatal message 日志输出");   // 会输出
#else
        LOG_TRACE(u"trace message 日志输出");   // 不会输出
        LOG_DEBUG(u"debug message 日志输出");   // 不会输出
        LOG_INFO(u"info message 日志输出");     // 会输出
        LOG_WARN(u"warn message 日志输出");     // 会输出
        LOG_ERROR(u"error message 日志输出");   // 会输出
        LOG_FATAL(u"fatal message 日志输出");   // 会输出
#endif

        // 设置线程名
        LikesProgram::CoreUtils::SetCurrentThreadName(u"主线程");
#ifdef _DEBUG
        LOG_TRACE(u"trace message 日志输出");   // 不会输出
        LOG_DEBUG(u"debug message 日志输出");   // 会输出
        LOG_INFO(u"info message 日志输出");     // 会输出
        LOG_WARN(u"warn message 日志输出");     // 会输出
        LOG_ERROR(u"error message 日志输出");   // 会输出
        LOG_FATAL(u"fatal message 日志输出");   // 会输出
#else
        LOG_TRACE(u"trace message 日志输出");   // 不会输出
        LOG_DEBUG(u"debug message 日志输出");   // 不会输出
        LOG_INFO(u"info message 日志输出");     // 会输出
        LOG_WARN(u"warn message 日志输出");     // 会输出
        LOG_ERROR(u"error message 日志输出");   // 会输出
        LOG_FATAL(u"fatal message 日志输出");   // 会输出
#endif
        std::this_thread::sleep_for(std::chrono::seconds(1)); // 给后台线程一点时间输出
        logger.Shutdown();
	}
}
```

### 7、ThreadPool：线程池 

`ThreadPool` 提供了一个可配置、动态伸缩的线程池，支持任务排队、异步提交、任务拒绝策略及运行时统计信息。它适合处理高并发任务，并提供线程安全的操作接口。

#### 拒绝策略（RejectPolicy）

当任务队列满时，可采用以下策略：

* `Block` 阻塞等待队列有空位再入队
* `Discard` 丢弃新任务
* `DiscardOld` 丢弃最老任务（队头），然后入队新任务
* `Throw` 抛出异常，通知调用者

#### 关闭策略（ShutdownPolicy）

线程池停止时可采用不同策略：

* `Graceful` 不接收新任务，执行完队列及正在运行任务后退出
* `Drain` 立即拒绝新任务，但执行队列中已有任务，尽快退出（不清空队列）
* `CancelNow` 立即拒绝新任务并丢弃队列任务，尽快退出

#### 配置选项（Options）

可配置线程池行为，包括线程数量、队列容量、任务拒绝策略等：

* `coreThreads` 最小线程数，默认与硬件并发数一致
* `maxThreads` 最大线程数，默认与硬件并发数一致
* `queueCapacity` 任务队列容量
* `rejectPolicy` 任务入队拒绝策略
* `keepAlive` 空闲线程存活时间
* `allowDynamicResize` 是否允许动态扩容/回收线程
* `threadNamePrefix` 线程名前缀
* `exceptionHandler` 任务执行异常回调

#### 运行时统计信息（Statistics）

线程池提供详细的任务和线程运行统计：

* `submitted` 成功入队任务数
* `rejected` 被拒绝任务数
* `completed` 执行完成任务数
* `active` 正在执行任务数
* `aliveThreads` 存活工作线程数
* `largestPoolSize` 历史最大线程数
* `peakQueueSize` 任务队列峰值
* `lastSubmitTime` 最后提交任务时间
* `lastFinishTime` 最后完成任务时间
* `longestTaskTime` 最长任务耗时
* `arithmeticAverageTaskTime` 算术平均任务耗时
* `averageTaskTime` 指数移动平均任务耗时

#### 生命周期控制

* `ThreadPool(Options opts = Options())` 构造线程池实例，可传入自定义配置
* `~ThreadPool()` 析构函数，停止线程池并清理资源
* `void Start()` 启动线程池（可多次调用，仅第一次生效）
* `void Shutdown(ShutdownPolicy mode = ShutdownPolicy::Graceful)` 统一停止线程池
* `bool AwaitTermination(std::chrono::milliseconds timeout)` 等待线程池终止，timeout=0 表示非阻塞
* `void JoinAll()` 阻塞等待所有线程退出

#### 老接口兼容

* `void Stop()` 等同于 `Shutdown(ShutdownPolicy::Graceful)`，用于平滑停止线程池，保留老代码调用方式。
* `void ShutdownNow()` 等同于 `Shutdown(ShutdownPolicy::CancelNow)`，用于立即停止线程池并丢弃队列任务，保留老代码调用方式。

#### 提交任务

线程池支持多种任务提交方式：

* `Submit(F&& f, Args&&... args)` 提交有返回值的任务，返回 `std::future`
* `Post(F&& f, Args&&... args)` 提交无返回值任务
* `PostNoArg(std::function<void()> fn)` 提交无返回值、无参数的任务

任务提交遵循配置的 `RejectPolicy`，可在失败时通过 `exceptionHandler` 回调处理异常。

#### 查询与监控

* `size_t GetQueueSize() const` 当前任务队列大小
* `size_t GetActiveCount() const` 正在执行任务的线程数
* `size_t GetThreadCount() const` 活跃线程数
* `bool IsRunning() const` 线程池是否运行
* `Statistics Snapshot() const` 获取线程池快照统计信息
* `size_t IetRejectedCount() const` 被拒绝任务数量
* `size_t IetTotalTasksSubmitted() const` 总提交任务数
* `size_t IetCompletedCount() const` 完成任务数
* `size_t IetLargestPoolSize() const` 历史最大线程数
* `size_t IetPeakQueueSize() const` 队列峰值

#### 使用示例

```cpp
#pragma once
#include <LikesProgram/ThreadPool.hpp>
#include <LikesProgram/Logger.hpp>
#include <LikesProgram/String.hpp>

namespace ThreadPoolTest {
	void Test() {        // 初始化日志
        auto& logger = LikesProgram::Logger::Instance();
#ifdef _WIN32
        logger.SetEncoding(LikesProgram::String::Encoding::GBK);
#endif
        logger.SetLevel(LikesProgram::Logger::LogLevel::Trace);
        // 内置控制台输出 Sink
        logger.AddSink(LikesProgram::Logger::CreateConsoleSink()); // 输出到控制台

        LikesProgram::ThreadPool::Options optins = {
        2,   // 最小线程数
        4,   // 线程数上限
        2048,// 队列长度限制
        LikesProgram::ThreadPool::RejectPolicy::Block, // 任务拒绝策略
        std::chrono::milliseconds(100), // 空闲线程回收时间
        true, // 是否启用动态扩容、缩容
        };

        // 创建线程池
        // LikesProgram::ThreadPool pool(optins); // 使用自定义参数创建线程池
        LikesProgram::ThreadPool pool; // 使用默认参数创建线程池
        pool.Start();

        // 提交一些任务
        for (int i = 0; i < 30; i++) {
            // 提交无返回值无参数的任务
            pool.PostNoArg([i]() {
                LOG_DEBUG(u"PostNoArg：Hello from worker");
            });

            // 提交无返回值有参数的任务
            pool.Post([](LikesProgram::String message) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                LikesProgram::String out;
                out.Append(message);
                out.Append(u"：Hello from worker");
                LOG_DEBUG(out);
            }, u"Post");

            // 提交有返回值有参数的任务
            auto poolOut = pool.Submit([i](LikesProgram::String message) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                LOG_DEBUG(message);
                LikesProgram::String out;
                out.Append(message);
                out.Append(u"：[");
                out.Append(LikesProgram::String(std::to_string(i)));
                out.Append(u"] Out");
                return out;
            }, u"Submit");

            // 等待任务完成, 并获取结果（这里会拖慢任务效率）
            // LOG_WARN(poolOut.get());

            if (i % 10 == 0) {
                LOG_WARN(poolOut.get()); // 每隔10次输出一次 Submit 的运行结果

                // 获取快照统计信息
                LikesProgram::ThreadPool::Statistics stats = pool.Snapshot();
                LOG_WARN(stats.ToString());
            }
        }

        // 关闭线程池
        pool.Shutdown();
        if (pool.AwaitTermination(std::chrono::milliseconds(1000))) { // 等待线程池关闭
            LOG_WARN(u"线程池已关闭");
        } else {
            LOG_ERROR(u"线程池关闭超时");
        }

        // 获取快照统计信息
        LikesProgram::ThreadPool::Statistics stats = pool.Snapshot();
        LOG_WARN(stats.ToString());

        std::this_thread::sleep_for(std::chrono::seconds(1)); // 给后台线程一点时间输出
        logger.Shutdown();
	}
}
```

### 8、Config：配置管理
