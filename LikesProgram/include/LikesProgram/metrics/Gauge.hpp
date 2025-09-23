#pragma once
#include "../LikesProgramLibExport.hpp"
#include "Metrics.hpp"

namespace LikesProgram {
	namespace Metrics {
        class LIKESPROGRAM_API Gauge : public MetricsObject {
        public:
            Gauge(const LikesProgram::String& name, const LikesProgram::String& help = u"",
                const std::unordered_map<LikesProgram::String, LikesProgram::String>& labels = {});
            void Set(int64_t value);
            void SetEmaAlpha(double alpha);

            void Increment(int64_t value = 1.0);
            void Decrement(int64_t value = 1.0);
            int64_t Value() const;

            double EMA() const;
            double Average() const;

            int64_t Max() const;
            int64_t Min() const;

            LikesProgram::String Name() const override;
            std::unordered_map<LikesProgram::String, LikesProgram::String> Labels() const override;
            LikesProgram::String Help() const override;
            LikesProgram::String Type() const override;
            LikesProgram::String ToPrometheus() const override;
            LikesProgram::String ToJson() const override;

        private:
            std::atomic<int64_t> m_value{ 0 };
            std::atomic<int64_t> m_sum{ 0 };
            std::atomic<int64_t> m_count{ 0 };
            std::atomic<double> m_ema{ 0.0 };
            std::atomic<int64_t> m_max{ std::numeric_limits<int64_t>::max() };
            std::atomic<int64_t> m_min{ std::numeric_limits<int64_t>::min() };
            double m_alpha = 0.9;

            void UpdateStats(int64_t value);
        };
	}
}