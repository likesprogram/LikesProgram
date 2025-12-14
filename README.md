# LikesProgram

## 简介

LikesProgram 是一个 **现代 C++（C++20）通用基础设施库**，目标不是“做大而全的框架”，而是提供一组**工程中反复会用到、但标准库没给全、第三方库又太重**的基础组件。

它更像一个“工具箱”：每个模块都可以单独使用，也可以自然地组合在一起，适合写 **服务端程序、基础设施、工具链、长期维护的工程**。

项目强调：

* 明确的所有权与生命周期（避免隐式魔法）
* 可测试性（`include/test` 中提供大量使用示例）
* 接近 STL / 标准库的接口风格
* 跨平台（Windows / Linux）

---

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

---

## 模块概览

* [1. String（Unicode 字符串）](#1-stringunicode-字符串)
* [2. StringFormat（类型安全的格式化系统）](#2-stringformat类型安全的格式化系统)
* [3. Math（向量与数学工具）](#3-math向量与数学工具)
* [4. Metrics（指标系统）](#4-metrics指标系统)
* [5. Threading（线程池）](#5-threading线程池)
* [6. Time（时间工具）](#6-time时间工具)
* [7. Logger（日志系统）](#7-logger日志系统)
* [8. System / Utils（系统与通用工具）](#8-system--utils系统与通用工具)
* [9. Configuration（配置管理）](#9-configuration配置管理)

---

### 1. String（Unicode 字符串）

路径：`include/LikesProgram/String.hpp`

String 是 LikesProgram 的核心组件之一，提供 **工程级可用的 Unicode 字符串抽象**。

特性：

* 原生支持 UTF-8 / UTF-16 / UTF-32
* 提供高效、可缓存的编码转换能力
* Unicode 感知的 length / substr / case 转换
* 可直接用于日志、格式化、配置、指标等模块

设计目标是：
**在真实工程中可靠使用的 Unicode 字符串，而不是演示性质的封装**。

示例：

* `include/test/StringTest.hpp`
* `include/test/UnicodeTest.hpp`

---

### 2. StringFormat（类型安全的格式化系统）

路径：`include/LikesProgram/stringFormat/*`

StringFormat 是一个 **与 LikesProgram::String 深度集成的格式化系统**，用于构建类型安全、可扩展的字符串格式化能力。

特性：

* 支持类似 `{}` 的格式化语法
* 支持 Unicode 对齐、填充与宽度计算
* 支持按类型、按名称注册自定义格式化器

你可以为自定义类型定义格式化规则，而无需在业务代码中散落 `toString()`。

示例：

* `include/test/StringFormatTest.hpp`
* 语法说明：`LikesProgram String Format 规范 v0.1.md`

---

### 3. Math（向量与数学工具）

路径：`include/LikesProgram/math/*`

Math 模块提供一组 **面向工程场景的数学类型与算法**，补充 STL 未覆盖的常用能力。

特性：

* 向量类型：`Vector` / `Vector3` / `Vector4`
* 完整的算术与比较运算符
* Dot / Cross / Normalize / Rotate / Slerp 等常用操作
* 多数计算提供 epsilon 感知的安全版本

同时提供：

* `PercentileSketch`：高性能分位数统计结构
  适用于 Metrics / Summary 等统计场景

示例：

* `include/test/VectorTest.hpp`
* `include/test/Vector3Test.hpp`
* `include/test/PercentileSketchTest.hpp`

---

### 4. Metrics（指标系统）

路径：`include/LikesProgram/metrics/*`

Metrics 是一个 **面向长期运行服务的工程级指标系统**，而非简单计数工具。

支持的指标类型：

* `Counter`：单调递增计数
* `Gauge`：可增可减的瞬时值
* `Histogram`：桶统计
* `Summary`：分位数 + EMA + Min / Max
* `Registry`：指标注册与集中管理

特性：

* 线程安全
* 支持 Prometheus / JSON 导出
* 可与 ThreadPool 等模块自然集成

示例：

* `include/test/MetricsTest.hpp`

---

### 5. Threading（线程池）

路径：`include/LikesProgram/threading/*`

Threading 提供一个 **可观测、可配置的线程池实现**，适合用于服务端和基础设施代码。

特性：

* 可配置线程数与任务队列策略
* 支持任务拒绝与回退策略
* 内建 Metrics（任务数、拒绝数、队列长度、执行耗时）
* 支持 Observer 机制（`IThreadPoolObserver`）

这是一个面向工程可维护性的选择，而不是最小实现。

示例：

* `include/test/ThreadPoolTest.hpp`

---

### 6. Time（时间工具）

路径：`include/LikesProgram/time/*`

Time 模块提供统一、精度明确的时间相关工具。

组件：

* `Time`：统一时间表示
* `Timer`：高精度计时工具（纳秒级）

常用于：

* 性能统计
* Metrics 数据采集
* 调试与 profiling

示例：

* `include/test/TimerTest.hpp`

---

### 7. Logger（日志系统）

路径：`include/LikesProgram/Logger.hpp`

Logger 是一个 **轻量级、可扩展的日志系统**，适合在基础设施库中使用。

特性：

* 多级别日志支持
* 与 String / StringFormat 深度集成
* 不强绑定具体后端，便于嵌入其他系统

示例：

* `include/test/LoggerTest.hpp`

---

### 8. System / Utils（系统与通用工具）

路径：`include/LikesProgram/system/*`

System / Utils 提供一组工程中常见、但标准库未直接提供的系统级工具。

包括但不限于：

* 线程命名
* UUID 生成
* 本机 IP / MAC 信息获取

这些工具用于减少重复实现，而非形成独立框架。

---

### 9. Configuration（配置管理）

路径：`include/LikesProgram/Configuration.hpp`

Configuration 是一个 **结构上类似 JSON 的配置对象模型**，用于在程序运行期以统一、类型安全的方式组织和访问配置数据。

特性：

* 支持键值对象、数组以及基础类型（int、int64_t、double、bool、String）
* 支持任意层级的嵌套结构
* 提供线程安全的访问语义、显式类型转换与迭代器遍历能力
* 内置一个 **轻量级的 JSON 序列化与反序列化实现**，适用于基础配置读写场景

设计说明：

* 内置 JSON 支持以简洁、可控为目标，不追求完整规范覆盖
* 对于复杂或严格的配置解析需求，建议使用外部解析库并映射到 Configuration 对象中

示例：

* `include/test/ConfigurationTest.hpp`

---

## 关于 include/test

`include/test/*` 在 LikesProgram 中具有**非常明确的定位**：

> **示例优先，其次才是测试**。

它们的主要目的不是追求测试覆盖率，而是：

* 展示真实、推荐的使用方式
* 固化接口语义（行为即文档）
* 防止接口在重构中发生“悄然变味”

换句话说，`test` 目录就是 **README 的延伸部分**。

---

## 使用示例与 test 目录的关系说明（重要）

> **本 README 不再提供“可运行示例代码块”。**

原因很简单，也很工程化：

* LikesProgram 的接口在演进中
* `include/test/*` 中的代码是 **唯一权威、始终与实现同步的真实用法**
* README 中的“示例代码”一旦与 test 出现偏差，就会变成误导

因此，本 README 的职责被明确限定为：

* 解释每个模块**解决什么问题**
* 指出**应该查看哪个 test 文件来学习真实用法**
* 说明接口的**语义与设计约束**

而不是重复维护一套“看起来能用、但可能已经过期”的示例。

---

## 如何正确学习和使用 LikesProgram

### 先看 test，而不是 README 示例

对于任何模块，推荐的顺序是：

```
include/LikesProgram/xxx.hpp   // 接口定义
include/test/xxxTest.hpp       // 实际用法（权威）
README.md                      // 设计背景与模块关系
```

`include/test` 中的代码具备以下特性：

* 可以直接编译
* 使用的是当前版本的真实 API
* 展示的是**作者期望的使用方式**

---

## 为什么 README 不直接给完整示例代码

这是一个**刻意的设计选择**，而不是偷懒：

* LikesProgram 是基础设施库，而不是单文件 header-only 工具
* 接口往往需要与多个模块组合使用
* 脱离 test 场景的“简化示例”容易掩盖真实约束

如果你希望：

* 示例 **一定能编译**
* 示例 **不会因为版本更新而过期**

那么 **test 就是示例本身**。

---

## 项目状态说明

* 本项目仍在持续演进
* `include/test` 中的用法具有最高优先级
* README 保证描述语义与设计原则，但不承诺示例级 API 稳定

如果你在使用过程中发现：

* README 描述与 test 行为不一致

请以 **test 为准**，README 会随后更新。

---

> LikesProgram 的文档策略是：
> **行为在 test 中，思想在 README 中。**

## 许可证

本库采用 **BSD 3-Clause License**，详见 LICENSE 文件。
Licensed under the BSD 3-Clause License. See LICENSE for details.
