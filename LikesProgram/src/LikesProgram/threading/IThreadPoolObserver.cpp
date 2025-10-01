#include "../../../include/LikesProgram/threading/IThreadPoolObserver.hpp"
#include "../../../include/LikesProgram/time/Timer.hpp"
#include "../../../include/LikesProgram/metrics/Counter.hpp"
#include "../../../include/LikesProgram/metrics/Gauge.hpp"
#include "../../../include/LikesProgram/metrics/Summary.hpp"
#include "../../../include/LikesProgram/metrics/Registry.hpp"

namespace LikesProgram {
    ThreadPoolObserverBase::ThreadPoolObserverBase(const String& poolName, std::shared_ptr<Metrics::Registry> registry) {
        InitMetrics(poolName, registry); // ��ʼ��ָ�����
        Register(); // ע��ָ�����
    }

    ThreadPoolObserverBase::~ThreadPoolObserverBase() {
        Unregister(); // ж��ָ�����
    }

    void ThreadPoolObserverBase::OnTaskSubmitted(double queueSize) {
        // �������ύ����ļ���
        m_metrics.m_submittedCount->Increment();
        // ��¼���һ���ύ�����ʱ��
        auto now_ns = std::chrono::duration_cast<Time::Nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        m_metrics.m_lastSubmitTimeGauge->Set((double)now_ns);
        // ��ӵ�ǰ���г���
        m_metrics.m_queueSizeGauge->Set(queueSize);
        // ���·�ֵ���г���
        m_metrics.m_peakQueueGauge->Set(std::max(m_metrics.m_peakQueueGauge->Value(), queueSize));
    }

    void ThreadPoolObserverBase::OnTaskRejected() {
        // ���ӱ��ܾ�����ļ���
        m_metrics.m_rejectedCount->Increment();
    }

    void ThreadPoolObserverBase::OnTaskStarted() {
        // ��ӵ�ǰ�������
        m_metrics.m_activeTasks->Increment();
    }

    void ThreadPoolObserverBase::OnTaskCompleted(Time::Nanoseconds duration, double queueSize) {
        // ���������������
        m_metrics.m_completedCount->Increment();
        // ����������ʱ��
        auto now_ns = std::chrono::duration_cast<Time::Nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        m_metrics.m_lastFinishTimeGauge->Set((double)now_ns);
        // �������ʱ ת��Ϊ�� ����¼
        m_metrics.m_taskTimeSummary->Observe(Time::NsToS(duration.count()));
        // ���ٵ�ǰ�������
        m_metrics.m_activeTasks->Decrement();
        // ��С���г���
        m_metrics.m_queueSizeGauge->Set(queueSize);
    }

    void ThreadPoolObserverBase::OnThreadCountAdded() {
        // ���´����߳���
        m_metrics.m_aliveThreadsGauge->Increment();
        // ���·�ֵ�߳���
        m_metrics.m_largestPoolGauge->Set(std::max(m_metrics.m_largestPoolGauge->Value(), m_metrics.m_aliveThreadsGauge->Value()));
    }

    void ThreadPoolObserverBase::OnThreadCountRemoved() {
        // ���´����߳���
        m_metrics.m_aliveThreadsGauge->Decrement();
    }

    const ThreadPoolMetrics& ThreadPoolObserverBase::GetMetrics() const {
        return m_metrics;
    }

    void ThreadPoolObserverBase::InitMetrics(const String& poolName, std::shared_ptr<Metrics::Registry> registry) {
        m_metrics.m_registry = registry;
        
        m_metrics.m_submittedCount = std::make_shared<Metrics::Counter>(poolName + u"_submitted_total", u"Tasks submitted");
        m_metrics.m_rejectedCount = std::make_shared<Metrics::Counter>(poolName + u"_rejected_total", u"Tasks rejected");
        m_metrics.m_completedCount = std::make_shared<Metrics::Counter>(poolName + u"_completed_total", u"Tasks completed");

        m_metrics.m_activeTasks = std::make_shared<Metrics::Gauge>(poolName + u"_active_tasks", u"Active tasks");
        m_metrics.m_aliveThreadsGauge = std::make_shared<Metrics::Gauge>(poolName + u"_alive_threads", u"Alive worker threads");
        m_metrics.m_queueSizeGauge = std::make_shared<Metrics::Gauge>(poolName + u"_queue_size", u"Current queue size");
        m_metrics.m_largestPoolGauge = std::make_shared<Metrics::Gauge>(poolName + u"_largest_pool_size", u"Largest pool size");
        m_metrics.m_peakQueueGauge = std::make_shared<Metrics::Gauge>(poolName + u"_peak_queue_size", u"Peak queue size");
        m_metrics.m_lastSubmitTimeGauge = std::make_shared<Metrics::Gauge>(poolName + u"_last_submit_time", u"Last submit timestamp (s)");
        m_metrics.m_lastFinishTimeGauge = std::make_shared<Metrics::Gauge>(poolName + u"_last_finish_time", u"Last finish timestamp (s)");

        m_metrics.m_taskTimeSummary = std::make_shared<Metrics::Summary>(poolName + u"_task_time_seconds", 1000, u"Task execution time (s)");
    }

    void ThreadPoolObserverBase::Register() {
        if (!m_metrics.m_registry) return;
        m_metrics.m_registry->Register(m_metrics.m_submittedCount);
        m_metrics.m_registry->Register(m_metrics.m_rejectedCount);
        m_metrics.m_registry->Register(m_metrics.m_completedCount);
        m_metrics.m_registry->Register(m_metrics.m_activeTasks);
        m_metrics.m_registry->Register(m_metrics.m_aliveThreadsGauge);
        m_metrics.m_registry->Register(m_metrics.m_queueSizeGauge);
        m_metrics.m_registry->Register(m_metrics.m_largestPoolGauge);
        m_metrics.m_registry->Register(m_metrics.m_peakQueueGauge);
        m_metrics.m_registry->Register(m_metrics.m_lastSubmitTimeGauge);
        m_metrics.m_registry->Register(m_metrics.m_lastFinishTimeGauge);
        m_metrics.m_registry->Register(m_metrics.m_taskTimeSummary);
    }

    void ThreadPoolObserverBase::Unregister() {
        if (!m_metrics.m_registry) return;
        m_metrics.m_registry->Unregister(m_metrics.m_submittedCount->Name(), m_metrics.m_submittedCount->Labels());
        m_metrics.m_registry->Unregister(m_metrics.m_rejectedCount->Name(), m_metrics.m_rejectedCount->Labels());
        m_metrics.m_registry->Unregister(m_metrics.m_completedCount->Name(), m_metrics.m_completedCount->Labels());
        m_metrics.m_registry->Unregister(m_metrics.m_activeTasks->Name(), m_metrics.m_activeTasks->Labels());
        m_metrics.m_registry->Unregister(m_metrics.m_aliveThreadsGauge->Name(), m_metrics.m_aliveThreadsGauge->Labels());
        m_metrics.m_registry->Unregister(m_metrics.m_queueSizeGauge->Name(), m_metrics.m_queueSizeGauge->Labels());
        m_metrics.m_registry->Unregister(m_metrics.m_largestPoolGauge->Name(), m_metrics.m_largestPoolGauge->Labels());
        m_metrics.m_registry->Unregister(m_metrics.m_peakQueueGauge->Name(), m_metrics.m_peakQueueGauge->Labels());
        m_metrics.m_registry->Unregister(m_metrics.m_lastSubmitTimeGauge->Name(), m_metrics.m_lastSubmitTimeGauge->Labels());
        m_metrics.m_registry->Unregister(m_metrics.m_lastFinishTimeGauge->Name(), m_metrics.m_lastFinishTimeGauge->Labels());
        m_metrics.m_registry->Unregister(m_metrics.m_taskTimeSummary->Name(), m_metrics.m_taskTimeSummary->Labels());
    }
}