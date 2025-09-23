#pragma once
#include "../LikesProgramLibExport.hpp"
#include "Metrics.hpp"

namespace LikesProgram {
	namespace Metrics {
        class Gauge : public MetricsObject {
        public:
            Gauge(const LikesProgram::String& name, const LikesProgram::String& help = u"", double alpha = 0.9,
                const std::unordered_map<LikesProgram::String, LikesProgram::String>& labels = {});
            void Set(double value);

            void Increment(double value = 1.0);
            void Decrement(double value = 1.0);
            double Value() const;

            double EMA() const;
            double Average() const;

            double Max() const;
            double Min() const;

            LikesProgram::String Name() const override;
            std::unordered_map<LikesProgram::String, LikesProgram::String> Labels() const override;
            LikesProgram::String Help() const override;
            LikesProgram::String Type() const override;
            LikesProgram::String ToPrometheus() const override;
            LikesProgram::String ToJson() const override;

        private:
            std::atomic<double> m_value{ 0 };
            std::atomic<double> m_sum{ 0 };
            std::atomic<int64_t> m_count{ 0 };
            std::atomic<double> m_ema{ 0.0 };
            std::atomic<double> m_min{ std::numeric_limits<double>::infinity() };
            std::atomic<double> m_max{ -std::numeric_limits<double>::infinity() };
            double m_alpha = 0.9;

            void UpdateStats(double value);
        };
	}
}