# LikesProgram Tools

`tools` 目录保存可以随发布包交付的命令行工具，以及各模块安装包外部消费方验收工程。
发布工具默认由 `LIKESPROGRAM_BUILD_TOOLS` 构建；外部消费方验收工程通常在安装
LikesProgram 后单独通过 `CMAKE_PREFIX_PATH` 构建。

## likesprogram-doctor

`likesprogram-doctor` 用于验证源码树或安装后的 LikesProgram 包是否能被消费方正常加载和使用。

当前检查内容：

- Core 版本、字符串格式化和 JSON 转义。
- 已链接 Logging 时，验证包身份和异步 Sink 往返。
- 已链接 Config 时，验证包身份、`key=value` 与 JSON 解析。
- 已链接 Metrics 时，验证包身份、基础指标采样和 Registry 导出。
- 已链接 Threading 时，验证包身份、任务提交、异常隔离和关闭链路。
- 已链接 Net 时，验证包身份、Buffer/Address 基础能力和共享安全资源初始化入口。

常用命令：

```powershell
likesprogram-doctor
likesprogram-doctor --format json
likesprogram-doctor --require core --require logging --require config --require metrics --require threading --require net
likesprogram-doctor --require all
```

退出码：

- `0`：检查通过。
- `1`：命令行参数错误。
- `2`：至少一个诊断项失败。
- `3`：出现未预期运行时错误。

安装 LikesProgram 后，也可以把该工具作为外部消费方单独构建：

```powershell
cmake -S tools/likesprogram-doctor -B build-doctor -DCMAKE_PREFIX_PATH=C:\LikesProgramInstall
cmake --build build-doctor --config Release
```

## 模块 consumer-check

每个稳定模块都有一个独立的安装包外部消费方验收工程，用于验证
`find_package(LikesProgram)` 后能正常链接对应 `LikesProgram::<Module>` target。

当前工程：

- `likesprogram-core-consumer-check`：验证 Core 版本、`String::Format` 和 JSON 转义。
- `likesprogram-logging-consumer-check`：验证 Logging 包身份、外部自定义 Sink、启动/Flush/Shutdown。
- `likesprogram-config-consumer-check`：验证 Config 包身份、`key=value` 和 JSON 解析。
- `likesprogram-metrics-consumer-check`：验证 Metrics 包身份、Counter 注册和 Prometheus 导出。
- `likesprogram-threading-consumer-check`：验证 Threading 包身份、Submit/Post 和关闭统计。
- `likesprogram-net-consumer-check`：验证 Net 包身份、Buffer、TCP/UDP 派生 transport 和共享安全资源初始化。

安装 LikesProgram 后，可以这样单独构建：

```powershell
cmake -S tools/likesprogram-core-consumer-check -B build-core-consumer-check -DCMAKE_PREFIX_PATH=C:\LikesProgramInstall
cmake --build build-core-consumer-check --config Release

cmake -S tools/likesprogram-logging-consumer-check -B build-logging-consumer-check -DCMAKE_PREFIX_PATH=C:\LikesProgramInstall
cmake --build build-logging-consumer-check --config Release

cmake -S tools/likesprogram-config-consumer-check -B build-config-consumer-check -DCMAKE_PREFIX_PATH=C:\LikesProgramInstall
cmake --build build-config-consumer-check --config Release

cmake -S tools/likesprogram-metrics-consumer-check -B build-metrics-consumer-check -DCMAKE_PREFIX_PATH=C:\LikesProgramInstall
cmake --build build-metrics-consumer-check --config Release

cmake -S tools/likesprogram-threading-consumer-check -B build-threading-consumer-check -DCMAKE_PREFIX_PATH=C:\LikesProgramInstall
cmake --build build-threading-consumer-check --config Release

cmake -S tools/likesprogram-net-consumer-check -B build-net-consumer-check -DCMAKE_PREFIX_PATH=C:\LikesProgramInstall
cmake --build build-net-consumer-check --config Release
```
