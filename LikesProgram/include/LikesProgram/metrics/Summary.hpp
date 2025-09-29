#pragma once
#include "../LikesProgramLibExport.hpp"
#include "../math/PercentileSketch.hpp"
#include "Metrics.hpp"

namespace LikesProgram {
	namespace Metrics {
        class LIKESPROGRAM_API Summary : public MetricsObject {
        public:
            Summary(const LikesProgram::String& name,
                size_t maxWindow = 1000, const LikesProgram::String& help = u"",
                const std::map<LikesProgram::String, LikesProgram::String>& labels = {});
            Summary(const Summary& other);
            Summary& operator=(const Summary& other);
            Summary(Summary&& other) noexcept;
            Summary& operator=(Summary&& other) noexcept;
            ~Summary() override;

            void Observe(double value);

            double Quantile(double q) const; // 0.95 → p95

            int64_t Count() const;

            double Sum() const;

            void Reset() override;

            // 设置移动平均的alpha，只有alpha在(0, 1)之间才有效，无效则不计算移动平均，默认不计算移动平均
            void SetEMAAlpha(double alpha);

            double EMA() const;

            double Min() const;

            double Max() const;

            LikesProgram::String Name() const override;
            std::map<LikesProgram::String, LikesProgram::String> Labels() const override;
            LikesProgram::String Help() const override;
            LikesProgram::String Type() const override;
            LikesProgram::String ToPrometheus() const override;
            LikesProgram::String ToJson() const override;

        private:
            size_t m_maxWindow;

            struct SummaryImpl;
            SummaryImpl* m_impl;

            double m_alpha = -1;

            Math::PercentileSketch m_sketch;

            void UpdateStats(double value);
        };
	}
}