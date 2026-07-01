# LikesProgramMetrics

`LikesProgramMetrics` 是独立指标扩展包，只依赖 `LikesProgramCore`。

## 能力范围

- `Counter`：单调累计计数。
- `Gauge`：可增可减的瞬时值。
- `Histogram`：固定桶累计直方图，支持 Prometheus bucket/sum/count 导出。
- `Summary`：轻量百分位估算、sum/count/min/max 和可选 EMA。
- `Registry`：按 `name + labels` 注册、替换、查询和统一导出指标。

## 工业级使用约束

- Counter、Gauge、Histogram 和 Summary 的数值导出保持有限值，极端累计输入会按有限边界饱和，避免生成 `NaN`/`Inf`。
- Histogram 采样热路径只更新命中桶，累计桶在查询和导出时生成，适合桶数量较多的延迟分布场景。
- Summary 内部百分位估算器使用分片写入和 64 位样本计数，适合长期运行服务的多线程采样。
- `Registry` 导出会先复制指标对象快照，再调用各指标导出函数，避免在注册表锁内执行用户扩展指标逻辑。

## 使用方式

```cmake
target_link_libraries(MyApp PRIVATE LikesProgram::Metrics)
```

包入口：

```cpp
#include <LikesProgram/Metrics/Metrics.hpp>
```

真实用法见 `examples/MetricsExample.cpp`。

## 测试与 benchmark

- `tests/MetricsPackageTests.cpp`：功能、边界、并发采样、注册表并发导出和极值稳定性回归。
- `benchmarks/MetricsBenchmark.cpp`：Release 场景下 Counter/Gauge 原子热路径、Histogram 多桶采样、Summary 多线程写入和 Registry 导出的性能观察样例。

## 依赖边界

Metrics 不依赖 Logging、Config、Threading、Net、OpenSSL 或第三方 JSON/YAML/TOML 库。
`PercentileSketch` 已作为 Summary 的包内私有实现迁入 `src/include/metrics`，不作为公共 Math API 安装导出。
