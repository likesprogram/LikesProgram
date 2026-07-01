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

## 使用

```cpp
#include <LikesProgram/Threading/Threading.hpp>

LikesProgram::Threading::ThreadPool pool;
pool.Start();
auto value = pool.Submit([] { return 42; });
pool.Shutdown();
```
