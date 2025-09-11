#pragma once
#include "LikesProgramLibExport.hpp"
#include <chrono>
#include <string>
#include <atomic>
#include <thread>

namespace LikesProgram {
    // �߾��ȼ�ʱ��
    class LIKESPROGRAM_API Timer {
    public:
        using Duration = std::chrono::nanoseconds;
        explicit Timer(bool autoStart = false);
        
        // ��ʼ��ʱ
        static void Start();

        // ֹͣ��ʱ
        static Duration Stop(double alpha = 0.9);

        // ���ü�ʱ��
        static void ResetThread();
        static void ResetGlobal();

        // ���һ�� Stop() �ĺ�ʱ
        static Duration GetLastElapsed();

        // �߳� Stop() ���ۼƺ�ʱ
        static Duration GetTotalElapsed();

        // ȫ�� Stop() ���ۼƺ�ʱ
        static Duration GetTotalGlobalElapsed();

        // ��ʷ���ʱ
        static Duration GetLongestElapsed();

        // EMA ƽ����ʱ
        static Duration GetEMAAverageElapsed();

        // ����ƽ����ʱ (�߳���)
        static Duration GetArithmeticAverageElapsed();

        // ����ƽ����ʱ (ȫ��)
        static Duration GetArithmeticAverageGlobalElapsed();

        // ��ǰ�Ƿ��ڼ�ʱ
        static bool IsRunning();

        // ��ʱ����ת��Ϊ�ַ���
        static std::string ToString(Duration duration);
    private:
        // ��ȡ�߾�������ʱ��
        static int64_t NowNs();

        // �ֲ߳̾���ʱ��
        thread_local static inline int64_t m_startNs = 0;
        thread_local static inline int64_t m_lastNs = 0;
        thread_local static inline int64_t m_totalNs = 0;
        thread_local static inline int64_t m_count = 0;
        thread_local static inline bool m_running = false;

        // ȫ�ּ�ʱ�� (���̹߳���)
        static inline std::atomic<long long> totalNs_ { 0 }; // ȫ���ۼ�ʱ��
        static inline std::atomic<long long> count_{ 0 }; // �ܼ�ʱ����
        static inline std::atomic<long long> longestNs_ { 0 }; // ȫ���ʱ��
        static inline std::atomic<double> averageNs_ { 0 }; // ƽ��ʱ��
    };
}
