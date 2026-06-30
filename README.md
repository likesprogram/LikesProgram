# LikesProgram

LikesProgram 是一个 C++20 基础组件库。它采用“Core + 可选扩展包”的结构：先提供稳定、轻量、无第三方重依赖的公共基础能力，再按功能域拆成日志、配置、指标、线程、网络等独立包。

当前版本可用的模块有：

| 模块 | CMake target | 默认构建 | 用途 | 手册 |
| --- | --- | --- | --- | --- |
| LikesProgramCore | `LikesProgram::Core` | 是 | Unicode 字符串、格式化、时间、状态返回、平台信息、字节视图等基础能力 | [packages/LikesProgramCore/README.md](packages/LikesProgramCore/README.md) |
| LikesProgramLogging | `LikesProgram::Logging` | 否，需要手动开启 | 异步日志、控制台/文件输出、结构化 JSON Lines、上下文字段、失败重试、多进程文件写入、运行时配置与诊断 | [packages/LikesProgramLogging/README.md](packages/LikesProgramLogging/README.md) |
| LikesProgramConfig | `LikesProgram::Config` | 否，需要手动开启 | `key=value`、JSON、YAML、TOML、值树、类型安全读取、默认值回退和 Schema 校验 | [packages/LikesProgramConfig/README.md](packages/LikesProgramConfig/README.md) |

后续版本可能继续提供 `LikesProgramMetrics`、`LikesProgramThreading`、`LikesProgramThreadingMetrics`、`LikesProgramNet`、`LikesProgramNetTls` 等包。当前版本请只使用上表中的模块；如果开启当前版本暂不提供的包，CMake 会提示该包不可用。

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

const char* loggingVersion = LikesProgram::Logging::PackageVersion();
const char* configVersion = LikesProgram::Config::PackageVersion();
bool loggingAvailable = LikesProgram::Logging::PackageAvailable();
bool configAvailable = LikesProgram::Config::PackageAvailable();
```

## 目录结构

```text
LikesProgram/
  CMakeLists.txt                     # 总构建入口
  AGENTS.md                          # 兼容只读根 AGENTS 的工具，指向 .agents/AGENTS.md
  .agents/
    AGENTS.md                        # 协作者与 Vibe Coding 工具优先读取的约束正文
  cmake/                             # install 后给 find_package 使用的配置模板
  docs/
    plans/                           # 重构计划和路线说明
    progress/                        # TASKS、进度大屏和本地服务脚本
  packages/
    LikesProgramCore/                # 基础包，始终构建
    LikesProgramLogging/             # 日志扩展包，默认不构建
    LikesProgramConfig/              # 配置扩展包，默认不构建
  tools/
    likesprogram-doctor/             # 发布包与外部消费方诊断工具
```

生成目录如 `build*`、`out`、`.vs` 是本地构建产物，不是源码接口的一部分。

## 环境要求

最低要求：

- CMake 3.15 或更新版本
- 支持 C++20 的编译器
- Windows 推荐 Visual Studio 2022 MSVC；Linux/macOS 推荐 GCC 11+ 或 Clang 14+

当前源码没有引入第三方库。Core、Logging、Config 都只依赖 C++ 标准库和系统 API。

## 一分钟构建

如果你只是想先确认项目能编译，按下面做。

Windows PowerShell：

```powershell
cd C:\Users\TX2\Desktop\LikesProgramProjects\LikesProgram
cmake -S . -B build -DLIKESPROGRAM_BUILD_LOGGING=ON -DLIKESPROGRAM_BUILD_CONFIG=ON
cmake --build build --config Debug
ctest --test-dir build --output-on-failure -C Debug
```

Linux/macOS：

```bash
cd /path/to/LikesProgram
cmake -S . -B build -DLIKESPROGRAM_BUILD_LOGGING=ON -DLIKESPROGRAM_BUILD_CONFIG=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

说明：

- `Core` 总是构建。
- `Logging` 默认不构建，想使用日志包必须传入 `-DLIKESPROGRAM_BUILD_LOGGING=ON`。
- `Config` 默认不构建，想使用配置包必须传入 `-DLIKESPROGRAM_BUILD_CONFIG=ON`。
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
| `LIKESPROGRAM_BUILD_METRICS` | `OFF` | 预留选项，当前版本暂不提供 |
| `LIKESPROGRAM_BUILD_THREADING` | `OFF` | 预留选项，当前版本暂不提供 |
| `LIKESPROGRAM_BUILD_THREADING_METRICS` | `OFF` | 预留选项，当前版本暂不提供 |
| `LIKESPROGRAM_BUILD_NET` | `OFF` | 预留选项，当前版本暂不提供 |
| `LIKESPROGRAM_BUILD_NET_TLS` | `OFF` | 预留选项，当前版本暂不提供 |

构建静态库：

```powershell
cmake -S . -B build-static -DLIKESPROGRAM_BUILD_SHARED=OFF -DLIKESPROGRAM_BUILD_LOGGING=ON -DLIKESPROGRAM_BUILD_CONFIG=ON
cmake --build build-static --config Release
```

只构建 Core：

```powershell
cmake -S . -B build-core-only -DLIKESPROGRAM_BUILD_LOGGING=OFF -DLIKESPROGRAM_BUILD_CONFIG=OFF
cmake --build build-core-only --config Release
```

构建 Core + Logging，不构建测试和示例：

```powershell
cmake -S . -B build-min -DLIKESPROGRAM_BUILD_LOGGING=ON -DLIKESPROGRAM_BUILD_TESTS=OFF -DLIKESPROGRAM_BUILD_EXAMPLES=OFF
cmake --build build-min --config Release
```

## 测试和示例目标

开启 `LIKESPROGRAM_BUILD_TESTS=ON` 后会注册：

```text
LikesProgramCoreTests
LikesProgramLoggingTests    # 仅在 LIKESPROGRAM_BUILD_LOGGING=ON 时存在
LikesProgramConfigTests     # 仅在 LIKESPROGRAM_BUILD_CONFIG=ON 时存在
```

运行全部测试：

```powershell
ctest --test-dir build --output-on-failure -C Debug
```

只运行某一个测试：

```powershell
ctest --test-dir build -R LikesProgramLoggingTests --output-on-failure -C Debug
```

开启 `LIKESPROGRAM_BUILD_EXAMPLES=ON` 后会生成：

```text
LikesProgramCoreExample
LikesProgramCoreBenchmark
LikesProgramLoggingExample      # 仅在 LIKESPROGRAM_BUILD_LOGGING=ON 时存在
LikesProgramLoggingBenchmark    # 仅在 LIKESPROGRAM_BUILD_LOGGING=ON 时存在
LikesProgramConfigExample   # 仅在 LIKESPROGRAM_BUILD_CONFIG=ON 时存在
```

这些示例就是当前 API 的可编译使用样本。README 负责解释“怎么用”和“为什么这样用”，examples/tests 负责证明代码真的能编译运行。

## 发布诊断工具

开启 `LIKESPROGRAM_BUILD_TOOLS=ON` 后会生成 `likesprogram-doctor`。它用于检查当前构建或安装后的包是否能正常被消费方使用：

```powershell
likesprogram-doctor
likesprogram-doctor --format json
likesprogram-doctor --require all
```

`--require all` 会把 Core、Logging、Config 都视为必须通过的组件；如果只构建 Core，保持默认命令即可，未链接的可选包会显示为 skipped。

工具的详细说明位于 [tools/README.md](tools/README.md)。

## 安装

安装到指定目录：

```powershell
cmake -S . -B build-install -DLIKESPROGRAM_BUILD_LOGGING=ON -DLIKESPROGRAM_BUILD_CONFIG=ON
cmake --build build-install --config Release
cmake --install build-install --prefix C:\LikesProgramInstall --config Release
```

安装后目录大致如下：

```text
C:\LikesProgramInstall\
  bin\                         # 动态库 dll，动态构建时存在
                               # likesprogram-doctor 也会安装到这里
  lib\                         # import lib 或静态库
  lib\cmake\LikesProgram\       # find_package 配置文件
  include\LikesProgram\         # 公开头文件
```

动态库构建时，运行程序需要能找到对应 dll。Windows 上最简单的方式是把 `bin` 放到 `PATH`，或把 dll 复制到 exe 同目录。项目内测试和示例已经在 Windows 动态构建下自动复制所需 dll。

## 在你的项目中使用

推荐方式是先安装，然后在外部项目里使用 `find_package`。

外部项目 `CMakeLists.txt`：

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
)
```

配置外部项目时告诉 CMake 安装目录：

```powershell
cmake -S . -B build -DCMAKE_PREFIX_PATH=C:\LikesProgramInstall
cmake --build build --config Release
```

最小 `main.cpp`：

```cpp
#include <LikesProgram/Core/String.hpp>
#include <LikesProgram/Config/Config.hpp>
#include <LikesProgram/Logging/Logging.hpp>

#include <chrono>
#include <source_location>

int main() {
    LikesProgram::String text = LikesProgram::String::Format(u"Hello, {}", u"LikesProgram");

    auto config = LikesProgram::Config::Configuration::FromJson(
        u"{"
        u"\"service\":{\"name\":\"demo\",\"port\":8080},"
        u"\"logging\":{\"level\":\"info\",\"format\":\"text\"}"
        u"}");

    auto& logger = LikesProgram::Log::Logger::Instance(true, true);
    logger.SetLoggerName(config.GetString(u"service.name", u"unknown"));
    logger.SetLevel(LikesProgram::Log::StringToLevel(
        config.GetString(u"logging.level", u"info"),
        LikesProgram::Log::Level::Info));
    logger.AddSink(LikesProgram::Log::ConsoleSink::CreateSink());
    logger.Log(LikesProgram::Log::Level::Info, std::source_location::current(),
        u"{} listens on {}", text, config.GetInt64(u"service.port", 80));
    logger.Flush(std::chrono::seconds(5));
    logger.Shutdown();

    return 0;
}
```

## 模块怎么选

只需要字符串、时间、状态返回、平台信息：链接 `LikesProgram::Core`。

需要写日志：配置 LikesProgram 时先打开 `-DLIKESPROGRAM_BUILD_LOGGING=ON`，然后链接 `LikesProgram::Logging`。Logging 会自动公开依赖 Core，所以消费方通常只写 `LikesProgram::Logging` 也可以获得 Core 的 include/link 传递依赖；为了表达清楚，也可以显式同时链接 `Core`。

需要读取配置文件：构建时打开 `-DLIKESPROGRAM_BUILD_CONFIG=ON`，然后链接 `LikesProgram::Config`。Config 支持 `key=value`、JSON、YAML、TOML、嵌套值树和 `ConfigSchema` 校验。

需要用配置驱动日志：同时打开 `LIKESPROGRAM_BUILD_CONFIG` 和 `LIKESPROGRAM_BUILD_LOGGING`，应用层读取配置后映射到 Logging 的开放式 `LoggerConfig`。Logging 包自身仍只依赖 Core，不会反向依赖 Config。

当前版本只需要按实际功能开启模块即可。只用基础能力时保持默认 Core；需要日志或配置时再分别开启 `LIKESPROGRAM_BUILD_LOGGING`、`LIKESPROGRAM_BUILD_CONFIG`。

## 推荐包含方式

二次开发时建议只包含安装目录中的 `LikesProgram/...` 头文件，例如：

```cpp
#include <LikesProgram/Core/String.hpp>
#include <LikesProgram/Logging/Logging.hpp>
#include <LikesProgram/Config/Config.hpp>
```

`src/include` 下的头文件服务于库自身构建和测试，不属于安装后的常规使用入口。外部项目使用上面的公开头即可。

## ABI 和二进制兼容

LikesProgram 对外提供 C++ API。C++ ABI 受编译器、标准库、运行库、架构、Debug/Release 配置影响。

请保持以下条件一致：

- 同一编译器系列和版本范围
- 同一 C++ 标准，当前是 C++20
- 同一运行库设置
- 同一 CPU 架构
- 同一 Debug/Release 配置

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
