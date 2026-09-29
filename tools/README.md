# LikesProgram Tools

`tools` 目录保存可以随发布包交付的命令行工具，以及各模块安装包外部消费方验收工程。
发布工具默认由 `LIKESPROGRAM_BUILD_TOOLS` 构建；外部消费方验收工程通常在安装
LikesProgram 后单独通过 `CMAKE_PREFIX_PATH` 构建。

## likesprogram-doctor

`likesprogram-doctor` 用于验证源码树或安装后的 LikesProgram 包是否能被消费方正常加载和使用。

当前检查内容：

- Core 版本、字符串格式化和 JSON 转义。
- 已链接 Logging 时，验证包身份和异步 Sink 往返。
- 已链接 Config 时，验证包身份、`key=value` 与 JSON5 解析。
- 已链接 Metrics 时，验证包身份、基础指标采样和 Registry 导出。
- 已链接 Threading 时，验证包身份、任务提交、异常隔离和关闭链路。
- 已链接 Net 时，验证包身份、Buffer/Address、背压、EventLoopGroup、连接池边界、
  Poller backend 诊断和 TLS/DTLS Engine 共享资源初始化入口；不会把平台私有
  Poller 类型或 `src/include` 头文件作为消费方依赖。
- 已链接 Http 时，验证包身份、HTTP/1 报文、HTTP/2/3 帧 codec，以及 HttpSession 默认不接网络的边界。

常用命令：

```powershell
likesprogram-doctor
likesprogram-doctor --format json
likesprogram-doctor --require core --require logging --require config --require metrics --require threading --require net --require http
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
- `likesprogram-config-consumer-check`：验证 Config 包身份、`key=value`、JSON5、YAML、TOML 往返和 Schema 校验。
- `likesprogram-metrics-consumer-check`：验证 Metrics 包身份、Counter 注册和 Prometheus 导出。
- `likesprogram-threading-consumer-check`：验证 Threading 包身份、Submit/Post 和关闭统计。
- `likesprogram-net-consumer-check`：验证 Net 包身份、Buffer、TCP/UDP 公开连接入口和共享 TLS/DTLS 资源初始化。
- `likesprogram-http-consumer-check`：验证 Http 包身份、HTTP/1 请求往返、HTTP/2/3 帧往返和用户注入的 Session Transport/Handler。

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

cmake -S tools/likesprogram-http-consumer-check -B build-http-consumer-check -DCMAKE_PREFIX_PATH=C:\LikesProgramInstall
cmake --build build-http-consumer-check --config Release
```

## Config 成熟解析器对标

`run-config-reference-benchmark.py` 重复运行 `LikesProgramConfigReferenceBenchmark`，保留预热和正式样本原始日志，并生成 measurements CSV、summary/comparisons JSON、ratio SVG、manifest 和 HTML 总览。JSON 同时对标 nlohmann/json 与 RapidJSON，YAML 对标 yaml-cpp；任一同口径平均耗时未持平时，runner 默认返回非零退出码并在 manifest 中记录 `failed_performance_parity`。

参考解析器只在显式设置 `LIKESPROGRAM_BUILD_CONFIG_REFERENCE_BENCHMARKS=ON` 时查找和链接，不会进入 Config 产品库或安装包。已构建参考目标后，可执行：

```bash
python3 tools/run-config-reference-benchmark.py \
    --benchmark build/packages/LikesProgramConfig/LikesProgramConfigReferenceBenchmark \
    --output-root docs/progress/performance \
    --label linux-release-static \
    --git-commit "$(git rev-parse HEAD)"
```

## 原始性能测试记录

`performance-test-records.py` 扫描性能结果目录中的 `test-record.json`，生成可离线查看的总看板和原始数据导出：

```powershell
python tools/performance-test-records.py `
    --records-root docs/progress/performance `
    --output-dir docs/progress/performance
```

生成产物包括 `index.html`、`test-records.csv/json`、`test-samples.csv/json` 和 `rejected-records.json`。HTML 与 CSV/JSON 使用同一份收集结果，不会平滑、舍入或按指标好坏筛选原始值。

每轮完整测试应提供 `test-record.json`：

- `completed=true` 只表示计划中的测试阶段完整执行；`outcome=passed` 与 `outcome=failed` 都会进入正式记录。
- `raw_files` 或 `raw_globs` 列出必须存在的原始日志、CSV 和元数据；任一文件缺失时整轮拒绝。
- `tabular_files` 或 `tabular_globs` 只指定需要逐行展开的 CSV；其他原始日志仍保留下载链接。
- 编译未通过、程序中断、测试提前退出或采样不完整的目录不得伪造 `completed=true`，其 manifest 会进入 `rejected-records.json`，不会混入测试趋势数据。

`schema_version=1` 只兼容已有历史结果。新运行器必须写 `schema_version=2`，并在 `execution` 中记录 `exit_code`、计划/完成阶段数、计划/观测时长、最低/实际资源采样数；只有阶段完整、观测时长达标、采样密度达标且时间范围有效的轮次才进入正式记录。`exit_code` 始终作为原始事实保留，非零退出但完整执行的失败轮次不会因此被过滤。默认资源采样最低密度为计划测量秒数的 90%，用于容纳 `sleep 1` 之外真实 `/proc` 采集开销，不会插值、补点或重写原始 CSV；可通过 `LP_HTTP_RESOURCE_MINIMUM_SAMPLE_PERCENT` 显式收紧。重复 `record_id` 的所有冲突轮次会同时拒绝，避免原始行串档。

`run-net-nginx-http-benchmark.sh` 会在退出清理后原子写入 v2 manifest，并刷新性能目录根部的总看板。归档目录没有 `.git` 时，应通过 `LP_HTTP_TEST_VERSION` 传入验证提交，同时用 `LP_HTTP_TEST_TYPE`、`LP_HTTP_TEST_PLATFORM` 和 `LP_HTTP_TEST_BACKEND` 固定看板筛选维度。请求错误会把结果标为 `failed` 并继续完成剩余计划阶段；完整执行后的非零退出码同样原样保留。工具崩溃、中断、提前结束或资源采样不足仍保留原始文件和拒绝诊断，但不会进入正式测试数据。

长稳 timeout 诊断可设置 `LP_HTTP_BENCH_SEGMENT_SECONDS`（正整数秒）。该值小于 `LP_HTTP_BENCH_DURATION_SECONDS` 时，运行器保持同一服务进程和连接配置，将每个 `wrk` 阶段拆成固定窗口，并额外写入 `qps-latency-segments.csv` 与带 `.segment-XXXX` 后缀的原始日志；CSV 中的 `started_at`、`finished_at` 和 `offset_seconds` 用于定位错误所在窗口。`error_count` 继续保留总错误数，同时写入 `connect_error_count`、`read_error_count`、`write_error_count`、`timeout_error_count` 和 `non_success_error_count`，可以区分连接、socket、请求超时与 HTTP 状态失败。默认值为 `0`，表示保持单段压测和既有汇总口径；分段不会改变计划阶段数或最低资源采样规则。

性能结果默认不自动轮转，保证总看板可以持续覆盖全部版本和测试类型，并保留完整失败轮次及 partial 原始证据。只有操作者明确设置正数 `LP_HTTP_BENCH_KEEP_RUNS` 时，运行器才会在新一轮开始前按时间戳目录执行容量轮转；`0` 表示禁用自动删除。
