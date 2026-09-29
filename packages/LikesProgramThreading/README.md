# LikesProgramThreading

`LikesProgramThreading` 提供独立线程池能力，只依赖 `LikesProgramCore`。

## 范围

- `ThreadPool`
- `RejectPolicy` / `ShutdownPolicy`
- `Statistics` 快照
- `IThreadPoolObserver` / `ThreadPoolObserverBase`

## Metrics 兼容设计

Threading 不包含、也不链接 Metrics。线程池只暴露稳定观察者事件与统计快照：

- `OnTaskSubmitted`
- `OnTaskRejected`
- `OnTaskStarted`
- `OnTaskCompleted`
- `OnThreadCountAdded`
- `OnThreadCountRemoved`

Metrics 模块侧或用户代码可以实现这些观察者事件，将事件映射到 Counter、Gauge、Summary 等指标。当前不创建独立 `LikesProgramThreadingMetrics` 模块。

`Statistics::lastSubmitTime` 与 `lastFinishTime` 是诊断用系统墙钟采样：第 1 个事件和之后每 64 个事件更新一次，最多落后 63 个提交或完成事件。它们不提供单调性保证，也不应用于任务耗时或超时判断；任务耗时继续使用观察者事件中的单调时钟 duration，并且不采样。

## 使用

```cpp
#include <LikesProgram/Threading/Threading.hpp>

LikesProgram::Threading::ThreadPool pool;
pool.Start();
auto value = pool.Submit([] { return 42; });
pool.Shutdown();
```
