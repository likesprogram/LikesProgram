# LikesProgram

## 介绍
`LikesProgram` 是一个 **C++ 通用工具库**，提供丰富、高效且易用的类库和工具函数，帮助开发者在各种项目中减少重复代码、提升开发效率，并保证性能和可扩展性。
本库面向多种场景，包括性能分析、数学计算、日志管理、多线程任务调度、国际化字符串处理等，设计时注重易用性和可扩展性。

## 功能概览
```
LikesProgram
├─ Math（数学工具）
│  ├─ 常量：PI、EPSILON、INFINITY
│  ├─ 函数：UpdateMax、EMA、NsToMs、NsToS、MsToNs、SToNs
│  ├─ Vector（二位向量）
│  ├─ Vector3（三维向量）
│  └─ Vector4（四维向量）
├─ Timer（高精度计时器）
│  ├─ 单线程计时
│  ├─ 多线程计时
│  └─ 时间转换与字符串输出
├─ Logger（日志系统）
│  ├─ 多级别日志
│  └─ 多种输出方式
├─ ThreadPool（线程池管理）
├─ String（国际化字符串）
│  ├─ UTF-8 / UTF-16 / UTF-32 支持
│  └─ 编码转换工具
├─ CoreUtils（系统与辅助工具）
│  ├─ 获取本机 MAC 地址
│  ├─ 获取本机 IP 地址
│  └─ 生成 UUID
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

### 2、String：国际化字符串工具 (未完成)

### 3、Timer：高精度计时器

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

### 4、Logger：灵活的日志系统 (未完成)
### 5、ThreadPool：线程池 (未完成)
### 6、CoreUtils：辅助工具类 (未完成)
