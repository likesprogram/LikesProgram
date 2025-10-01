#pragma once
#include "../LikesProgramLibExport.hpp"
#include "../String.hpp"
#include "../time/Time.hpp"
#include "../metrics/Counter.hpp"
#include "../metrics/Gauge.hpp"
#include "../metrics/Summary.hpp"
#include "../metrics/Registry.hpp"

namespace LikesProgram {
    struct ThreadPoolMetrics {
        String m_poolName; // ָ�������̳߳�����ǰ׺���������ֲ�ͬ�̳߳� (������ threadNamePrefix ��һ��)
        std::shared_ptr<Metrics::Registry> m_registry = nullptr; // ע���ָ�룬����ע��/ע��ָ��

        // ������ (Counter)
        std::shared_ptr<Metrics::Counter> m_submittedCount; // �ɹ��ύ����������
        std::shared_ptr<Metrics::Counter> m_rejectedCount;  // ���ܾ�����������
        std::shared_ptr<Metrics::Counter> m_completedCount; // �ɹ���ɵ���������

        // ����ָ�� (Gauge)
        std::shared_ptr<Metrics::Gauge> m_queueSizeGauge; // ��ǰ������г���
        std::shared_ptr<Metrics::Gauge> m_peakQueueGauge; // ��ʷ�����г��ȣ���ֵ��

        // �߳�������״̬ (Gauge)
        std::shared_ptr<Metrics::Gauge> m_activeTasks;       // ��ǰ����ִ�е�������
        std::shared_ptr<Metrics::Gauge> m_aliveThreadsGauge; // ��ǰ���Ĺ����߳���
        std::shared_ptr<Metrics::Gauge> m_largestPoolGauge;  // ��ʷ����߳���

        // ʱ��ָ�� (Gauge)
        std::shared_ptr<Metrics::Gauge> m_lastSubmitTimeGauge; // ���һ�������ύ��ʱ������룩
        std::shared_ptr<Metrics::Gauge> m_lastFinishTimeGauge; // ���һ��������ɵ�ʱ������룩

        // ��ʱͳ�� (Summary)
        std::shared_ptr<Metrics::Summary> m_taskTimeSummary; // ����ִ�к�ʱ�ֲ�����λ���룩��֧��ƽ��ֵ����λ����ͳ��
    };

    class LIKESPROGRAM_API IThreadPoolObserver {
    public:
        virtual ~IThreadPoolObserver() = default;

        // �����ύʱ�Ļص�
        virtual void OnTaskSubmitted(double queueSize) = 0;
        // ���񱻾ܾ�ʱ�Ļص�
        virtual void OnTaskRejected() = 0;
        // ����ʼִ��ʱ�Ļص�
        virtual void OnTaskStarted() = 0;
        // �������ʱ�Ļص�
        virtual void OnTaskCompleted(Time::Nanoseconds duration, double queueSize) = 0;
        // ���������߳�ʱ�Ļص�
        virtual void OnThreadCountAdded() = 0;
        // �����̼߳���ʱ�Ļص�
        virtual void OnThreadCountRemoved() = 0;
        // ��ȡע����
        virtual const ThreadPoolMetrics& GetMetrics() const = 0;
    protected:
    };

    class LIKESPROGRAM_API ThreadPoolObserverBase : public IThreadPoolObserver {
        public:
        ThreadPoolObserverBase(const String& poolName, std::shared_ptr<Metrics::Registry> registry);
        virtual ~ThreadPoolObserverBase();

        // �����ύʱ�Ļص�
        virtual void OnTaskSubmitted(double queueSize);
        // ���񱻾ܾ�ʱ�Ļص�
        virtual void OnTaskRejected();
        // ����ʼִ��ʱ�Ļص�
        virtual void OnTaskStarted();
        // �������ʱ�Ļص�
        virtual void OnTaskCompleted(Time::Nanoseconds duration, double queueSize);
        // ���������߳�ʱ�Ļص�
        virtual void OnThreadCountAdded();
        // �����̼߳���ʱ�Ļص�
        virtual void OnThreadCountRemoved();
        // ��ȡע����
        virtual const ThreadPoolMetrics& GetMetrics() const;
    protected:
        void InitMetrics(const String& poolName, std::shared_ptr<Metrics::Registry> registry);
        void Register();
        void Unregister();
        ThreadPoolMetrics m_metrics;
    };
}