#pragma once
#include "../LikesProgramLibExport.hpp"
#include "Metrics.hpp"
#include <deque>
#include <mutex>

namespace LikesProgram {
	namespace Metrics {
        class LIKESPROGRAM_API Summary : public MetricsObject {
        public:
            Summary(const LikesProgram::String& name, const LikesProgram::String& help = u"",
                const std::unordered_map<LikesProgram::String, LikesProgram::String>& labels = {});

            void Observe(double v);

            LikesProgram::String Name() const override;
            std::unordered_map<LikesProgram::String, LikesProgram::String> Labels() const override;
            LikesProgram::String Help() const override;
            LikesProgram::String Type() const override;
            LikesProgram::String ToPrometheus() const override;
            LikesProgram::String ToJson() const override;

        private:
            void UpdateStats(double value);

            size_t m_maxWindow;
            std::deque<double> m_window;
            mutable std::mutex m_mutex;

            std::atomic<int64_t> m_count{ 0 };
            std::atomic<double> m_sum{ 0.0 };
            std::atomic<double> m_ema{ 0.0 };
            std::atomic<double> m_min{ std::numeric_limits<double>::infinity() };
            std::atomic<double> m_max{ -std::numeric_limits<double>::infinity() };
            double m_alpha = 0.9;
        };
	}
}