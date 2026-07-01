#pragma once
#include <LikesProgram/Metrics/Metric.hpp>

#include <cstdint>
#include <cstddef>

namespace LikesProgram {
    namespace Metrics {
        // 摘要指标，维护滑动百分位估算和基础统计值。
        class LIKESPROGRAM_METRICS_API Summary : public Metric {
        public:
            // 创建 Summary，maxWindow 控制内部 sketch 压缩窗口规模。
            Summary(const LikesProgram::String& name, size_t maxWindow = 1000,
                const LikesProgram::String& help = u"",
                const std::map<LikesProgram::String, LikesProgram::String>& labels = {});
            // 深拷贝 Summary 的统计状态和 sketch。
            Summary(const Summary& other);
            // 深拷贝赋值 Summary 的统计状态和 sketch。
            Summary& operator=(const Summary& other);
            // 移动接管 Summary 实现对象。
            Summary(Summary&& other) noexcept;
            // 移动赋值 Summary 实现对象。
            Summary& operator=(Summary&& other) noexcept;
            // 释放 Summary 实现对象。
            ~Summary() override;

            // 记录一次样本。
            void Observe(double value);
            // 查询分位数，q 必须位于 [0, 1]。
            double Quantile(double q) const;
            // 返回样本总数。
            int64_t Count() const;
            // 返回样本值总和。
            double Sum() const;

            // 清空所有统计状态。
            void Reset() override;

            // 设置指数移动平均 alpha，只有 (0, 1) 范围会启用 EMA。
            void SetEMAAlpha(double alpha);
            // 返回当前指数移动平均值。
            double EMA() const;
            // 返回已观察样本最小值，无样本时返回 double 最大值。
            double Min() const;
            // 返回已观察样本最大值，无样本时返回 double 最小值。
            double Max() const;

            // 返回指标名称。
            LikesProgram::String Name() const override;
            // 返回标签副本。
            std::map<LikesProgram::String, LikesProgram::String> Labels() const override;
            // 返回帮助文本。
            LikesProgram::String Help() const override;
            // 返回 summary 类型标识。
            LikesProgram::String Type() const override;
            // 导出 Prometheus summary 文本。
            LikesProgram::String ToPrometheus() const override;
            // 导出 summary JSON 对象。
            LikesProgram::String ToJson() const override;
        private:
            struct SummaryImpl;
            SummaryImpl* m_impl = nullptr; // 统计值和分位数估算私有实现

            // 更新 count/sum/min/max/EMA 等基础统计值。
            void UpdateStats(double value);
        };
    }
}
