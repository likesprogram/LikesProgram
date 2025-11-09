#pragma once
#include "../system/LikesProgramLibExport.hpp"
#include "../String.hpp"
#include "../time/Time.hpp"
#include "../metrics/Counter.hpp"
#include "../metrics/Gauge.hpp"
#include "../metrics/Summary.hpp"
#include "../metrics/Registry.hpp"

namespace LikesProgram {
    struct ThreadPoolMetrics {
        String m_poolName; // 指标所属线程池名称前缀，用于区分不同线程池 (建议与 threadNamePrefix 称一致)
        std::shared_ptr<Metrics::Registry> m_registry = nullptr; // 注册表指针，用于注册/注销指标

        // 计数器 (Counter)
        std::shared_ptr<Metrics::Counter> m_submittedCount; // 成功提交的任务总数
        std::shared_ptr<Metrics::Counter> m_rejectedCount;  // 被拒绝的任务总数
        std::shared_ptr<Metrics::Counter> m_completedCount; // 成功完成的任务总数

        // 队列指标 (Gauge)
        std::shared_ptr<Metrics::Gauge> m_queueSizeGauge; // 当前任务队列长度
        std::shared_ptr<Metrics::Gauge> m_peakQueueGauge; // 历史最大队列长度（峰值）

        // 线程与任务状态 (Gauge)
        std::shared_ptr<Metrics::Gauge> m_activeTasks;       // 当前正在执行的任务数
        std::shared_ptr<Metrics::Gauge> m_aliveThreadsGauge; // 当前存活的工作线程数
        std::shared_ptr<Metrics::Gauge> m_largestPoolGauge;  // 历史最大线程数

        // 时间指标 (Gauge)
        std::shared_ptr<Metrics::Gauge> m_lastSubmitTimeGauge; // 最近一次任务提交的时间戳（秒）
        std::shared_ptr<Metrics::Gauge> m_lastFinishTimeGauge; // 最近一次任务完成的时间戳（秒）

        // 耗时统计 (Summary)
        std::shared_ptr<Metrics::Summary> m_taskTimeSummary; // 任务执行耗时分布（单位：秒），支持平均值、分位数等统计
    };

    class LIKESPROGRAM_API IThreadPoolObserver {
    public:
        virtual ~IThreadPoolObserver() = default;

        // 任务提交时的回调
        virtual void OnTaskSubmitted(double queueSize) = 0;
        // 任务被拒绝时的回调
        virtual void OnTaskRejected() = 0;
        // 任务开始执行时的回调
        virtual void OnTaskStarted() = 0;
        // 任务完成时的回调
        virtual void OnTaskCompleted(Time::Nanoseconds duration, double queueSize) = 0;
        // 新增工作线程时的回调
        virtual void OnThreadCountAdded() = 0;
        // 工作线程减少时的回调
        virtual void OnThreadCountRemoved() = 0;
        // 获取注册器
        virtual const ThreadPoolMetrics& GetMetrics() const = 0;
    protected:
    };

    class LIKESPROGRAM_API ThreadPoolObserverBase : public IThreadPoolObserver {
        public:
        ThreadPoolObserverBase(const String& poolName, std::shared_ptr<Metrics::Registry> registry);
        virtual ~ThreadPoolObserverBase();

        // 任务提交时的回调
        virtual void OnTaskSubmitted(double queueSize);
        // 任务被拒绝时的回调
        virtual void OnTaskRejected();
        // 任务开始执行时的回调
        virtual void OnTaskStarted();
        // 任务完成时的回调
        virtual void OnTaskCompleted(Time::Nanoseconds duration, double queueSize);
        // 新增工作线程时的回调
        virtual void OnThreadCountAdded();
        // 工作线程减少时的回调
        virtual void OnThreadCountRemoved();
        // 获取注册器
        virtual const ThreadPoolMetrics& GetMetrics() const;
    protected:
        void InitMetrics(const String& poolName, std::shared_ptr<Metrics::Registry> registry);
        void Register();
        void Unregister();
        ThreadPoolMetrics m_metrics;
    };
}