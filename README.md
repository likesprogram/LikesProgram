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
└─ Configuration（配置管理）
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
* `Vector FromPolar(double length, double angle)`：根据极坐标生成向量，其中 length 是向量长度，angle 是与 X 轴的角度（弧度）

#### 构造函数

* `Vector::Zero()` → `(0,0)`：零向量。
* `Vector::One()` → `(1,1)`：所有分量为 1。
* `Vector::UnitX()` → `(1,0)`：X 轴单位向量。
* `Vector::UnitY()` → `(0,1)`：Y 轴单位向量。

#### 运算符重载

* **标量运算**：

  * `v + s` / `v - s` / `v * s` / `v / s`
  * `v += s` / `v -= s` / `v *= s` / `v /= s`

* **向量运算**：

  * `v + u` / `v - u`
  * `v * u`（分量乘）
  * `v / u`（分量除）
  * 对应的 `+=`、`-=``、`\*=`、`/=\` 复合赋值操作。

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

##### 基本属性

  * `double Length() const`：返回向量长度
  * `double LengthSquared() const`：返回长度平方
  * `Vector Normalized() const`：返回单位向量（长度为 1），原向量不变
  * `void Normalize()`：就地归一化向量，使长度为 1
  * `Vector SafeNormalized(double epsilon = 1e-9) const`：安全归一化：长度小于 `epsilon` 返回零向量，否则返回单位向量
  * `bool IsNormalized(double epsilon = 1e-9) const`：判断长度是否接近 1（在误差范围内）
  * `bool IsZero(double epsilon = 1e-9) const`：判断向量长度是否接近 0
  * `Vector WithLength(length)`：返回方向不变，但长度为指定 `length` 的新向量
  * `Vector Clamped(maxLength)`：长度超过 `maxLength` 若长度超过 `maxLength`，按比例缩放到该长度，否则返回原向量

##### 代数运算

  * `double Dot(const Vector& v) const` / `static double Dot(const Vector& a, const Vector& b)`：点积 `x1*x2 + y1*y2`，可用作投影或夹角计算
  * `double Cross(const Vector& v) const`：二维叉积（标量）：`x1*y2 - y1*x2`，用于判断方向或旋转方向
  * `double Distance(const Vector& v) const`：两向量欧氏距离
  * `double DistanceSquared(const Vector& v) const`：距离平方，避免开方
  * `Vector Abs() const`：每个分量取绝对值，返回新向量
  * `Vector Min(const Vector& v) const`：按分量取最小值组成新向量
  * `Vector Max(const Vector& v) const`：按分量取最大值组成新向量
  * `bool NearlyEquals(const Vector& v, double epsilon = 1e-9) const`：判断向量是否“几乎相等”，误差不超过 `epsilon`

##### 代数运算

  * `Vector Perpendicular() const`：返回垂直向量（逆时针 90°）
  * `Vector Reflected(const Vector& normal) const`：沿单位法线反射向量，返回新向量
  * `Vector Project(const Vector& on) const`：将当前向量投影到向量 `on` 上
  * `Vector Reject(const Vector& on) const`：去掉投影部分，得到正交向量

##### 插值

  * `static Vector Lerp(const Vector& a, const Vector& b, double t)`：线性插值 `(1-t)*a + t*b`，`t ∈ [0,1]`。
  * `static Vector Slerp(const Vector& a, const Vector& b, double t)`：圆弧插值（球面线性插值），保持向量长度随角度变化。



#### 使用示例

```cpp
#include <iostream>
#include <random>
#include <LikesProgram/math/Vector.hpp>
#include <LikesProgram/math/Math.hpp>
#include <LikesProgram/Timer.hpp>

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

		delete data;
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
* 拼接运算：`operator+=` / `operator+`
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

        LikesProgram::String sAdd1 = u"LikesProgram 字符串 - " + s1;
        LikesProgram::String sAdd2 = s1 + u" - LikesProgram 字符串";
#ifdef _WIN32
        std::cout << "sAdd1: " << sAdd1.ToStdString(LikesProgram::String::Encoding::GBK) << "\n";
        std::cout << "sAdd2: " << sAdd2.ToStdString(LikesProgram::String::Encoding::GBK) << "\n";
#else
        std::cout << "sAdd1: " << sAdd1 << "\n";
        std::cout << "sAdd2: " << sAdd2 << "\n";
#endif

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

### 8、Configuration：配置管理

`Configuration` 类提供灵活的配置管理功能，支持键值对象（类似 JSON 对象）、数组、基础类型（int、int64\_t、double、bool、String）以及嵌套结构。它封装了线程安全访问、类型转换、迭代器遍历和序列化功能，适合用于应用配置、参数管理以及数据交换。

#### 核心类型定义

* `using ObjectMap = std::map<String, Configuration>`：对象键值对类型
* `using Array = std::vector<Configuration>`：数组类型
* `using Value = std::variant<std::monostate, int, int64_t, double, bool, String, Array, ObjectMap>`：值类型
* `enum class CastPolicy { Strict, AutoConvert }`：类型转换策略

  * `Strict` 严格转换，不允许隐式类型转换
  * `AutoConvert` 自动转换，例如字符串 `"123"` 可转换为 int

#### 构造与赋值

* `Configuration()` 默认构造，创建空配置
* `Configuration(int v)` / `int64_t` / `double` / `bool` / `String` / C 字符串 / 单字符
* `Configuration(Array v)` 构造数组
* `Configuration(ObjectMap v)` 构造对象
* `Configuration(const Configuration& other)` 拷贝构造
* `Configuration(Configuration&& other) noexcept` 移动构造
* 拷贝赋值 / 移动赋值 / 各类型赋值运算符 `operator=`

#### 类型判断

* `IsNull()` 是否为空
* `IsInt()` / `IsInt64()` / `IsDouble()` / `IsBool()` / `IsString()` / `IsArray()` / `IsObject()` / `IsNumber()` 判断类型
* `TypeName()` 返回类型名称（字符串）

#### 访问与修改

* 对象访问：

  * `operator[](const String& key)` 可读写
  * `const operator[](const String& key) const` 只读
* 数组访问：

  * `operator[](size_t idx)` / `At(size_t idx)` 安全访问
  * `Size()` 返回数组元素个数
* 添加或修改元素：

  * `Emplace(const String& key, Configuration val)` 向对象添加键值
  * `Push_back(Configuration val)` 向数组添加元素

#### 迭代器支持

* 数组迭代器：`begin()` / `end()` / `begin() const` / `end() const`
* 对象迭代器：`beginObject()` / `endObject()` / `beginObject() const` / `endObject() const`
* 对象范围支持 range-for：

  * `ObjectRange objects()`
  * `ConstObjectRange objects() const`

#### 类型转换

* `AsInt(CastPolicy p = AutoConvert)` 转换为 int
* `AsInt64(CastPolicy p = AutoConvert)` 转换为 int64\_t
* `AsDouble(CastPolicy p = AutoConvert)` 转换为 double
* `AsBool(CastPolicy p = AutoConvert)` 转换为 bool
* `AsString(CastPolicy p = AutoConvert)` 转换为 String
* `AsArray(CastPolicy p = Strict)` / `AsObject(CastPolicy p = Strict)` 获取数组或对象引用
* `TryGet(T& out, CastPolicy p = AutoConvert)` 安全获取值，失败返回 false

#### 比较运算

* `operator==` / `operator!=` 比较配置值是否相等

#### 序列化与反序列化

* 内置 `Serializer` 接口，可自定义序列化策略

  * `Serialize(const Configuration&, int indent)` 序列化为文本
  * `Deserialize(const String&)` 解析文本为 Configuration
* 默认提供 JSON 序列化器：`CreateJsonSerializer()`
* 设置默认序列化器：

  * `SetDefaultSerializer(std::shared_ptr<Serializer>)`
* 单对象设置序列化器：

  * `SetSerializer(std::shared_ptr<Serializer>)`
* 输出：

  * `Dump(int indent = 2)` 生成序列化文本
  * `Load(const String& text)` 加载序列化文本
  
> **注意事项**：
>
> * 推荐使用 `SetDefaultSerializer` 设置统一序列化器，避免不同对象使用不同序列化器导致输出格式紊乱。
> * JSON 序列化器默认支持缩进，可通过 `Dump(indent)` 调整。
> * 反序列化时，序列化器类型必须一致，否则可能读取失败。

#### 使用示例

```cpp
#include <LikesProgram/Configuration.hpp>
#include <LikesProgram/String.hpp>
#include <LikesProgram/Logger.hpp>
#include <iostream>
#include <sstream>
#include <vector>

namespace LikesProgram {

    /*
     * SimpleSerializer 示例说明
     *
     * 在本示例中，我们实现了一个简单的 Serializer，序列化成类似：
     * user_name=Alice
     * user_id=987654321012345
     * address.street=456 Park Ave
     * hobbies.0=reading
     *
     */

    class SimpleSerializer : public Configuration::Serializer {
    public:
        String Serialize(const Configuration& cfg, int indent = 2) const override {
            std::ostringstream oss;
            SerializeRec(cfg, "", oss);
            return String(oss.str().c_str());
        }

        Configuration Deserialize(const String& text) const override {
            Configuration cfg;
            std::istringstream iss(text.ToStdString());
            std::string line;
            while (std::getline(iss, line)) {
                auto eq_pos = line.find('=');
                if (eq_pos == std::string::npos) continue; // 忽略不合法行
                std::string path = line.substr(0, eq_pos);
                std::string value = line.substr(eq_pos + 1);
                SetValueByPath(cfg, path, value);
            }
            return cfg;
        }

    private:
        void SerializeRec(const Configuration& cfg, const std::string& prefix, std::ostringstream& oss) const {
            if (cfg.IsObject()) {
                for (auto& [k, v] : cfg.AsObject()) {
                    std::string newPrefix = prefix.empty() ? k.ToStdString() : prefix + "." + k.ToStdString();
                    SerializeRec(v, newPrefix, oss);
                }
            }
            else if (cfg.IsArray()) {
                const auto& arr = cfg.AsArray();
                for (size_t i = 0; i < arr.size(); ++i) {
                    std::string newPrefix = prefix + "." + std::to_string(i);
                    SerializeRec(arr[i], newPrefix, oss);
                }
            }
            else { // 基本类型
                std::string val;
                if (cfg.IsBool()) val = cfg.AsBool() ? "true" : "false";
                else if (cfg.IsInt()) val = std::to_string(cfg.AsInt());
                else if (cfg.IsInt64()) val = std::to_string(cfg.AsInt64());
                else if (cfg.IsDouble()) val = std::to_string(cfg.AsDouble());
                else if (cfg.IsString()) val = cfg.AsString().ToStdString();
                else val = "";
                oss << prefix << "=" << val << "\n";
            }
        }

        void SetValueByPath(Configuration& cfg, const std::string& path, const std::string& value) const {
            size_t dotPos = path.find('.');
            if (dotPos == std::string::npos) {
                // 最底层 key
                cfg[String(path)] = value.c_str();
            }
            else {
                std::string head = path.substr(0, dotPos);
                std::string tail = path.substr(dotPos + 1);
                // 判断是否数组索引
                bool isArray = !tail.empty() && std::all_of(tail.begin(), tail.end(), [](char c) { return std::isdigit(c) || c == '.'; });
                if (std::isdigit(tail[0])) { // 数组
                    int idx = std::stoi(tail.substr(0, tail.find('.')));
                    Configuration& arr = cfg[String(head)];
                    while (arr.Size() <= idx) arr.Push_back(Configuration());
                    SetValueByPath(arr[idx], tail.substr(tail.find('.') + 1), value);
                }
                else { // 对象
                    SetValueByPath(cfg[String(head)], tail, value);
                }
            }
        }
    };

    inline std::shared_ptr<Configuration::Serializer> CreateSimpleSerializer() {
        return std::make_shared<SimpleSerializer>();
    }
}


namespace ConfigurationTest {
	void Test() {
        auto& logger = LikesProgram::Logger::Instance();
#ifdef _WIN32
        logger.SetEncoding(LikesProgram::String::Encoding::GBK);
#endif
        logger.SetLevel(LikesProgram::Logger::LogLevel::Trace);
        // 内置控制台输出 Sink
        logger.AddSink(LikesProgram::Logger::CreateConsoleSink()); // 输出到控制台

        // 创建顶层配置对象
        LikesProgram::Configuration cfg;

        // 对象直接赋值
        cfg[u"user_name"] = u"Alice";
        cfg[u"user_id"] = int64_t(987654321012345);

        // 使用 emplace 添加键值
        cfg.Emplace(u"is_active", true);

        // 嵌套对象
        LikesProgram::Configuration address;
        address[u"street"] = u"456 Park Ave";
        address[u"city"] = u"Shanghai";
        address[u"zip"] = 200000;
        cfg[u"address"] = address;

        // 数组操作
        LikesProgram::Configuration::Array hobbies; // 明确指定为数组类型
        hobbies.push_back(u"reading"); // LikesProgram::Configuration::Array 本质上就是 std::vector 因此需要使用 push_back
        hobbies.push_back(u"gaming");
        hobbies.push_back(u"traveling");
        // 直接添加键值
        //cfg["hobbies"] = std::move(hobbies);
        // 使用 emplace 添加键值
        cfg.Emplace(u"hobbies", std::move(hobbies));

        // 多层嵌套对象和数组
        LikesProgram::Configuration projects;
        LikesProgram::Configuration projectList; // 自动识别为数组

        LikesProgram::Configuration proj1;
        proj1[u"name"] = u"LikesProgram - C++ 通用库";
        proj1[u"stars"] = 0;
        projectList.Push_back(proj1); // LikesProgram::Configuration 中需要使用 Push_back 添加数组元素

        LikesProgram::Configuration proj2;
        proj2[u"name"] = u"MyCoolApp";
        proj2[u"stars"] = 1500;
        projectList.Push_back(proj2);

        projects[u"list"] = projectList;
        cfg[u"projects"] = projects;

        // 类型转换示例
        try {
            int zip = cfg[u"address"][u"zip"].AsInt();
            int64_t userId = cfg[u"user_id"].AsInt64();
            LikesProgram::String street = cfg[u"address"][u"street"].AsString();
            bool active = cfg[u"is_active"].AsBool();
            
            LikesProgram::String out = u"User ID: ";
            out += LikesProgram::String(std::to_string(userId));
            out += u", Street: "; out += street;
            out += u", Zip: "; out += LikesProgram::String(std::to_string(zip));
            out += u", Active: "; out += LikesProgram::String(active ? u"true" : u"false");
            LOG_DEBUG(out);
        }
        catch (std::exception& e) {
            LikesProgram::String out = u"Conversion error: ";
            out += (LikesProgram::String)e.what();
            LOG_ERROR(out);
        }

        // CastPolicy 示例
        try {
            double userIdDouble = cfg[u"user_id"].AsDouble(LikesProgram::Configuration::CastPolicy::Strict);
            LikesProgram::String out = u"User ID as double: ";
            out += (LikesProgram::String)std::to_string(userIdDouble);
            LOG_DEBUG(out);
        }
        catch (std::exception& e) {
            LikesProgram::String out = u"Strict cast error: ";
            out += (LikesProgram::String)e.what();
            LOG_ERROR(out);
        }

        // 安全获取 tryGet
        int stars = 0;
        if (cfg[u"projects"][u"list"][1][u"stars"].TryGet(stars)) {
            LikesProgram::String out = u"Project 2 stars: ";
            out += (LikesProgram::String)std::to_string(stars);
            LOG_DEBUG(out);
        }

        // 遍历对象
        LOG_WARN(u"User info : ");
        for (auto it = cfg.beginObject(); it != cfg.endObject(); ++it) {
            LikesProgram::String out = it->first;
            out += u": "; out += it->second.AsString();
            LOG_DEBUG(out);
        }

        // 遍历数组
        LOG_WARN(u"Hobbies : ");
        for (const auto& hobby : cfg[u"hobbies"]) {
            LikesProgram::String out = u"- ";
            out += hobby.AsString();
            LOG_DEBUG(out);
        }
        // 遍历数组
        LOG_WARN(u"Hobbies : ");
        for (size_t i = 0; i < cfg[u"hobbies"].Size(); ++i) {
            LikesProgram::String out = u"- ";
            out += cfg[u"hobbies"].At(i).AsString();
            LOG_DEBUG(out);
        }

        // 遍历嵌套数组 + 对象
        LOG_WARN(u"nProjects : ");
        for (const auto& project : cfg[u"projects"][u"list"]) {
            LikesProgram::String out = u"- ";
            out += project[u"name"].AsString();
            out += u" ("; out += (LikesProgram::String)std::to_string(project[u"stars"].AsInt());
            out += u" stars)";
            LOG_DEBUG(out);
        }
        // JSON 序列化 / 反序列化

        // 设置默认的序列化器，通过该函数设置的序列化器，所有 Configuration 对象都可以使用，除非 你设置了 Configuration::SetSerializer
        // 默认的序列化器就是 JsonSerializer 除非你设置了其他序列化器，或是你需要自定义一个序列化器
        //LikesProgram::Configuration::SetDefaultSerializer(LikesProgram::Configuration::CreateJsonSerializer());

        // 使用默认的序列化器，获取 JSON 文本
        LikesProgram::String jsonText = cfg.Dump(4); // 缩进4空格
        LikesProgram::String jsonTextOut = u"Serialized JSON : \n";
        jsonTextOut += jsonText;
        LOG_DEBUG(jsonTextOut);

        // 使用默认的序列化器，反序列化
        LikesProgram::Configuration loadedCfg;
        loadedCfg.Load(jsonText);
        LikesProgram::String loadedOut = u"Loaded project 1 name: ";
        loadedOut += loadedCfg[u"projects"][u"list"][0][u"name"].AsString();
        LOG_DEBUG(loadedOut);

        // 异常捕获示例
        try {
            int invalid = loadedCfg[u"nonexistent"].AsInt(); // key 不存在
        }
        catch (std::exception& e) {
            LikesProgram::String out = u"Expected error (key missing): ";
            out += (LikesProgram::String)e.what();
            LOG_ERROR(out);
        }

        try {
            double invalidType = loadedCfg[u"user_name"].AsDouble(); // 类型不匹配
        }
        catch (std::exception& e) {
            LikesProgram::String out = u"Expected error (type mismatch): ";
            out += (LikesProgram::String)e.what();
            LOG_ERROR(out);
        }

        try {
            auto hobby = loadedCfg[u"hobbies"].At(10); // 越界
        }
        catch (std::exception& e) {
            LikesProgram::String out = u"Expected error (array out-of-range): ";
            out += (LikesProgram::String)e.what();
            LOG_ERROR(out);
        }

        // 使用自定义序列化器，注意：因为自定义序列化器是JSON，与上面使用的默认序列化器输出格式相同，因此才可以正常读取
        // 在正常开发中，需要注意：不同序列化器读取的配置文件不同
        // 在实际使用中，推荐使用 SetDefaultSerializer，不推荐给对象单独设置序列化器
        // 因为这样会造成输出格式紊乱，导致输出结果不可读
        cfg.SetSerializer(LikesProgram::CreateSimpleSerializer());
        LikesProgram::String simpleText = cfg.Dump(4); // 缩进4空格
        LikesProgram::String simpleTextOut = u"SimpleSerializer Text : \n";
        simpleTextOut += simpleText;
        LOG_DEBUG(simpleTextOut);

        LikesProgram::Configuration loadedCfg1;
        loadedCfg1.SetSerializer(LikesProgram::CreateSimpleSerializer());
        loadedCfg1.Load(simpleText);
        LikesProgram::String loadedOut1 = u"Loaded project 1 name: ";
        loadedOut1 += loadedCfg1[u"projects"][u"list"][0][u"name"].AsString();
        LOG_DEBUG(loadedOut1);

        std::this_thread::sleep_for(std::chrono::seconds(1)); // 给后台线程一点时间输出
        logger.Shutdown();
	}
}
```
