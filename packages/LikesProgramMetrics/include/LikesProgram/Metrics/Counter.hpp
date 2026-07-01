#pragma once
#include <LikesProgram/Metrics/Metric.hpp>

namespace LikesProgram {
    namespace Metrics {
        // 单调计数指标，适合累计请求数、错误数等事件计数。
        class LIKESPROGRAM_METRICS_API Counter : public Metric {
        public:
            // 创建 Counter，初始值为 0。
            Counter(const LikesProgram::String& name, const LikesProgram::String& help = u"",
                const std::map<LikesProgram::String, LikesProgram::String>& labels = {});
            // 深拷贝 Counter 当前值和元数据。
            Counter(const Counter& other);
            // 深拷贝赋值 Counter 当前值和元数据。
            Counter& operator=(const Counter& other);
            // 移动接管 Counter 实现对象。
            Counter(Counter&& other) noexcept;
            // 移动赋值 Counter 实现对象。
            Counter& operator=(Counter&& other) noexcept;
            // 释放 Counter 实现对象。
            ~Counter() override;

            // 增加计数值，负值会被忽略以保持 Counter 单调语义。
            void Increment(double value = 1.0);
            // 返回当前计数值。
            double Value() const;

            // 重置计数值为 0。
            void Reset() override;

            // 返回指标名称。
            LikesProgram::String Name() const override;
            // 返回标签副本。
            std::map<LikesProgram::String, LikesProgram::String> Labels() const override;
            // 返回帮助文本。
            LikesProgram::String Help() const override;
            // 返回 counter 类型标识。
            LikesProgram::String Type() const override;
            // 导出 Prometheus counter 文本。
            LikesProgram::String ToPrometheus() const override;
            // 导出 counter JSON 对象。
            LikesProgram::String ToJson() const override;
        private:
            struct CounterImpl;
            CounterImpl* m_impl = nullptr; // 原子计数值实现
        };
    }
}
