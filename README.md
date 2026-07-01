# LikesProgram

LikesProgram 是一个 C++20 基础组件库。它采用“Core + 可选扩展包”的结构：先提供稳定、轻量、无第三方重依赖的公共基础能力，再按功能域拆成日志、配置、指标、线程、网络等独立包。

当前版本可用的模块有：

| 模块 | CMake target | 默认构建 | 用途 | 手册 |
| --- | --- | --- | --- | --- |
| LikesProgramCore | `LikesProgram::Core` | 是 | Unicode 字符串、格式化、时间、状态返回、平台信息、字节视图等基础能力 | [packages/LikesProgramCore/README.md](packages/LikesProgramCore/README.md) |
| LikesProgramLogging | `LikesProgram::Logging` | 否，需要手动开启 | 异步日志、控制台/文件输出、结构化 JSON Lines、上下文字段、失败重试、多进程文件写入、运行时配置与诊断 | [packages/LikesProgramLogging/README.md](packages/LikesProgramLogging/README.md) |
| LikesProgramConfig | `LikesProgram::Config` | 否，需要手动开启 | `key=value`、JSON、YAML、TOML、值树、类型安全读取、默认值回退和 Schema 校验 | [packages/LikesProgramConfig/README.md](packages/LikesProgramConfig/README.md) |
| LikesProgramMetrics | `LikesProgram::Metrics` | 否，需要手动开启 | Counter、Gauge、Histogram、Summary、Registry 与 Prometheus/JSON 导出 | [packages/LikesProgramMetrics/README.md](packages/LikesProgramMetrics/README.md) |
| LikesProgramThreading | `LikesProgram::Threading` | 否，需要手动开启 | ThreadPool、有界队列、拒绝策略、关闭策略、统计快照与观察者事件 | [packages/LikesProgramThreading/README.md](packages/LikesProgramThreading/README.md) |
| LikesProgramNet | `LikesProgram::Net` | 否，需要手动开启 | TCP/UDP Server/Client、EventLoop、Connection、可继承 TcpTransport/UdpTransport 与用户自定义 TLS/SSL 扩展点 | [packages/LikesProgramNet/README.md](packages/LikesProgramNet/README.md) |

首轮未纳入新架构的能力已在 [legacy/README.md](legacy/README.md) 中登记边界；未来如需恢复，应重新按独立包规则设计、实现和验收。

## 版本信息

当前版本是 `1.0.0`。

在程序里可以通过 Core 提供的版本 API 读取当前库版本：

```cpp
#include <LikesProgram/Core/Version.hpp>

auto info = LikesProgram::Version::Current();
auto text = LikesProgram::Version::CurrentString(); // "1.0.0"

if (LikesProgram::Version::IsAtLeast(1, 0, 0)) {
    // 当前版本至少是 1.0.0
}
```

扩展包也提供包名、版本和可用性检查，便于二次开发时输出诊断信息：

```cpp
#include <LikesProgram/Logging/Logging.hpp>
#include <LikesProgram/Config/Config.hpp>
#include <LikesProgram/Metrics/Metrics.hpp>
#include <LikesProgram/Threading/Threading.hpp>
#include <LikesProgram/Net/Net.hpp>

const char* loggingVersion = LikesProgram::Logging::PackageVersion();
const char* configVersion = LikesProgram::Config::PackageVersion();
const char* metricsVersion = LikesProgram::Metrics::PackageVersion();
const char* threadingVersion = LikesProgram::Threading::PackageVersion();
const char* netVersion = LikesProgram::Net::PackageVersion();
```

## 环境要求

- CMake 3.15 或更新版本。
- 支持 C++20 的编译器。
- Windows 推荐 Visual Studio 2022 MSVC；Linux/macOS 推荐 GCC 11+ 或 Clang 14+。

当前源码没有引入第三方库。Core、Logging、Config、Metrics、Threading、Net 都只依赖 C++ 标准库和系统 API；Net 只提供可继承的 TCP/UDP transport 扩展点，TLS/SSL 由用户派生实现，不链接 OpenSSL。

首轮 1.0 版本不迁入宽泛 Math 聚合、Vector 系列或半成品 TLS Context Manager。`PercentileSketch` 已作为 Metrics 私有实现服务 `Summary`，不作为公共 Math API 导出。

## 一分钟构建

Windows PowerShell：

```powershell
cd C:\Users\TX2\Desktop\LikesProgramProjects\LikesProgram
cmake -S . -B build -DLIKESPROGRAM_BUILD_LOGGING=ON -DLIKESPROGRAM_BUILD_CONFIG=ON -DLIKESPROGRAM_BUILD_METRICS=ON -DLIKESPROGRAM_BUILD_THREADING=ON -DLIKESPROGRAM_BUILD_NET=ON
cmake --build build --config Debug
ctest --test-dir build --output-on-failure -C Debug
```

Linux/macOS：

```bash
cd /path/to/LikesProgram
cmake -S . -B build -DLIKESPROGRAM_BUILD_LOGGING=ON -DLIKESPROGRAM_BUILD_CONFIG=ON -DLIKESPROGRAM_BUILD_METRICS=ON -DLIKESPROGRAM_BUILD_THREADING=ON -DLIKESPROGRAM_BUILD_NET=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

说明：

- `Core` 总是构建。
- `Logging` 默认不构建，想使用日志包必须传入 `-DLIKESPROGRAM_BUILD_LOGGING=ON`。
- `Config` 默认不构建，想使用配置包必须传入 `-DLIKESPROGRAM_BUILD_CONFIG=ON`。
- `Metrics` 默认不构建，想使用指标包必须传入 `-DLIKESPROGRAM_BUILD_METRICS=ON`。
- `Threading` 默认不构建，想使用线程池包必须传入 `-DLIKESPROGRAM_BUILD_THREADING=ON`。
- `Net` 默认不构建，想使用 TCP/UDP 网络包必须传入 `-DLIKESPROGRAM_BUILD_NET=ON`。
- 测试由 `LIKESPROGRAM_BUILD_TESTS` 控制，默认 `ON`。
- 示例和 benchmark 由 `LIKESPROGRAM_BUILD_EXAMPLES` 控制，默认 `ON`。
- 发布诊断工具由 `LIKESPROGRAM_BUILD_TOOLS` 控制，默认 `ON`。

## 常用 CMake 选项

| 选项 | 默认值 | 作用 |
| --- | --- | --- |
| `LIKESPROGRAM_BUILD_SHARED` | `ON` | `ON` 构建动态库，`OFF` 构建静态库 |
| `LIKESPROGRAM_BUILD_TESTS` | `ON` | 构建并注册测试目标 |
| `LIKESPROGRAM_BUILD_EXAMPLES` | `ON` | 构建示例程序和 benchmark |
| `LIKESPROGRAM_BUILD_TOOLS` | `ON` | 构建并安装发布诊断工具 |
| `LIKESPROGRAM_BUILD_LOGGING` | `OFF` | 构建 Logging 扩展包 |
| `LIKESPROGRAM_BUILD_CONFIG` | `OFF` | 构建 Config 扩展包 |
| `LIKESPROGRAM_BUILD_METRICS` | `OFF` | 构建 Metrics 扩展包 |
| `LIKESPROGRAM_BUILD_THREADING` | `OFF` | 构建 Threading 扩展包 |
| `LIKESPROGRAM_BUILD_NET` | `OFF` | 构建 Net 扩展包，提供 TCP/UDP 与可继承安全层扩展点 |

## 测试和示例目标

开启 `LIKESPROGRAM_BUILD_TESTS=ON` 后会注册：

```text
LikesProgramCoreTests
LikesProgramLoggingTests    # 仅在 LIKESPROGRAM_BUILD_LOGGING=ON 时存在
LikesProgramConfigTests     # 仅在 LIKESPROGRAM_BUILD_CONFIG=ON 时存在
LikesProgramMetricsTests    # 仅在 LIKESPROGRAM_BUILD_METRICS=ON 时存在
LikesProgramThreadingTests  # 仅在 LIKESPROGRAM_BUILD_THREADING=ON 时存在
LikesProgramNetTests        # 仅在 LIKESPROGRAM_BUILD_NET=ON 时存在
```

开启 `LIKESPROGRAM_BUILD_EXAMPLES=ON` 后会生成：

```text
LikesProgramCoreExample
LikesProgramCoreBenchmark
LikesProgramLoggingExample      # 仅在 LIKESPROGRAM_BUILD_LOGGING=ON 时存在
LikesProgramLoggingBenchmark    # 仅在 LIKESPROGRAM_BUILD_LOGGING=ON 时存在
LikesProgramConfigExample       # 仅在 LIKESPROGRAM_BUILD_CONFIG=ON 时存在
LikesProgramMetricsExample      # 仅在 LIKESPROGRAM_BUILD_METRICS=ON 时存在
LikesProgramThreadingExample    # 仅在 LIKESPROGRAM_BUILD_THREADING=ON 时存在
LikesProgramNetExample          # 仅在 LIKESPROGRAM_BUILD_NET=ON 时存在
```

这些示例就是当前 API 的可编译使用样本。README 负责解释“怎么用”和“为什么这样用”，examples/tests 负责证明代码真的能编译运行。

## 在你的项目中使用

推荐方式是先安装，然后在外部项目里使用 `find_package`。

```cmake
cmake_minimum_required(VERSION 3.15)
project(MyApp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(LikesProgram CONFIG REQUIRED)

add_executable(MyApp main.cpp)
target_link_libraries(MyApp
    PRIVATE
        LikesProgram::Core
        LikesProgram::Logging
        LikesProgram::Config
        LikesProgram::Metrics
        LikesProgram::Threading
        LikesProgram::Net
)
```

配置外部项目时告诉 CMake 安装目录：

```powershell
cmake -S . -B build -DCMAKE_PREFIX_PATH=C:\LikesProgramInstall
cmake --build build --config Release
```

## 模块怎么选

只需要字符串、时间、状态返回、平台信息：链接 `LikesProgram::Core`。

需要写日志：构建时打开 `-DLIKESPROGRAM_BUILD_LOGGING=ON`，然后链接 `LikesProgram::Logging`。

需要读取配置文件：构建时打开 `-DLIKESPROGRAM_BUILD_CONFIG=ON`，然后链接 `LikesProgram::Config`。

需要记录运行时指标：构建时打开 `-DLIKESPROGRAM_BUILD_METRICS=ON`，然后链接 `LikesProgram::Metrics`。

需要线程池：构建时打开 `-DLIKESPROGRAM_BUILD_THREADING=ON`，然后链接 `LikesProgram::Threading`。

需要 TCP/UDP 网络：构建时打开 `-DLIKESPROGRAM_BUILD_NET=ON`，然后链接 `LikesProgram::Net`。Net 支持 `Server` / `Client` 选择 `TransportKind::Tcp` 或 `TransportKind::Udp`；TLS/SSL 由用户继承 `TcpTransport` 或 `UdpTransport`，重写安全层初始化、升级、握手和读写函数，并通过 factory 注入。

需要用配置驱动日志：同时打开 `LIKESPROGRAM_BUILD_CONFIG` 和 `LIKESPROGRAM_BUILD_LOGGING`，应用层读取配置后映射到 Logging 的开放式 `LoggerConfig`。Logging 包自身仍只依赖 Core，不会反向依赖 Config。

## ABI 和二进制兼容

LikesProgram 对外提供 C++ API。C++ ABI 受编译器、标准库、运行库、架构、Debug/Release 配置影响。

请保持以下条件一致：

- 同一编译器系列和版本范围。
- 同一 C++ 标准，当前是 C++20。
- 同一运行库设置。
- 同一 CPU 架构。
- 同一 Debug/Release 配置。

跨工具链使用时建议从源码重新构建，避免运行库和标准库 ABI 不一致带来的链接或运行问题。

## 编码约定

`LikesProgram::String` 内部统一保存 UTF-16。对外可以从 UTF-8、UTF-16、UTF-32、`std::wstring`、GBK 等来源构造，并可再转回对应编码。

建议：

- 新代码优先使用 UTF-8 源文本或 `u"..."` UTF-16 字面量。
- 日志和配置里的文本都使用 `LikesProgram::String`。
- 非法 UTF-8/UTF-16/UTF-32 输入会按严格策略抛出异常，调用外部输入时要在边界处处理异常。

## 开源协议

历史工程声明 LikesProgram 使用 Apache License 2.0。你可以在遵守 Apache License 2.0 条款的前提下使用、复制、修改和分发本项目，也可以用于商业项目。

需要注意：

- 保留原始版权声明和许可证声明。
- 修改后的文件应标注修改。
- 不得使用作者、项目名或贡献者名义为衍生项目背书。
- 本项目按“现状”提供，不承诺适用性、稳定性或安全性。
- 第三方依赖如果未来引入，仍受其各自许可证约束。

如果你的源码包或安装包中随附了 `LICENSE`、`NOTICE` 等文件，请以这些文件为准查看完整授权文本和声明信息。
