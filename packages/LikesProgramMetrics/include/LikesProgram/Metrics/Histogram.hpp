#pragma once
#include <LikesProgram/Metrics/Metric.hpp>
#include <LikesProgram/Core/time/Timer.hpp>

#include <cstdint>
#include <vector>

namespace LikesProgram {
    namespace Metrics {
        // 固定桶直方图指标，适合延迟和大小分布统计。
        class LIKESPROGRAM_METRICS_API Histogram : public Metric {
        public:
            // 创建 Histogram，桶边界会排序并去除 NaN。
            Histogram(const LikesProgram::String& name, const std::vector<double>& buckets,
                const LikesProgram::String& help = u"",
                const std::map<LikesProgram::String, LikesProgram::String>& labels = {});
            // 深拷贝桶、计数和元数据。
            Histogram(const Histogram& other);
            // 深拷贝赋值桶、计数和元数据。
            Histogram& operator=(const Histogram& other);
            // 移动接管 Histogram 实现对象。
            Histogram(Histogram&& other) noexcept;
            // 移动赋值 Histogram 实现对象。
            Histogram& operator=(Histogram&& other) noexcept;
            // 释放 Histogram 实现对象。
            ~Histogram() override;

            // 记录一次样本，单位由调用方语义决定，常用秒。
            void Observe(double value);
            // 记录 Timer 最近一次停止的耗时，单位为秒。
            void ObserveDuration(const LikesProgram::Time::Timer& timer);

            // 返回桶边界副本。
            std::vector<double> Buckets() const;
            // 返回每个桶的累计计数副本。
            std::vector<int64_t> Counts() const;
            // 返回总样本数。
            int64_t Count() const;
            // 返回样本值总和。
            double Sum() const;

            // 清空桶计数、总数和总和。
            void Reset() override;

            // 返回指标名称。
            LikesProgram::String Name() const override;
            // 返回标签副本。
            std::map<LikesProgram::String, LikesProgram::String> Labels() const override;
            // 返回帮助文本。
            LikesProgram::String Help() const override;
            // 返回 histogram 类型标识。
            LikesProgram::String Type() const override;
            // 导出 Prometheus histogram 文本。
            LikesProgram::String ToPrometheus() const override;
            // 导出 histogram JSON 对象。
            LikesProgram::String ToJson() const override;
        private:
            struct HistogramImpl;
            HistogramImpl* m_impl = nullptr; // 桶边界、桶计数和累计值实现
        };
    }
}
