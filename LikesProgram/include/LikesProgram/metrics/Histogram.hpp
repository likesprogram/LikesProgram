#pragma once
#include "../LikesProgramLibExport.hpp"
#include "Metrics.hpp"
#include "../Timer.hpp"

namespace LikesProgram {
	namespace Metrics {
		class Histogram : public MetricsObject {
		public:
			Histogram(const LikesProgram::String& name, const std::vector<double>& buckets,
				double alpha = 0.9, const LikesProgram::String& help = u"",
				const std::unordered_map<LikesProgram::String, LikesProgram::String>& labels = {});

			// value 单位为 纳秒
			void Observe(double value);
			void ObserveDuration(const Timer& timer);

			double EMA() const;
			double Average() const;

			double Max() const;
			double Min() const;

			LikesProgram::String Name() const override;
			std::unordered_map<LikesProgram::String, LikesProgram::String> Labels() const override;
			LikesProgram::String Help() const override;
			LikesProgram::String Type() const override { return u"histogram"; }
			LikesProgram::String ToPrometheus() const override;
			LikesProgram::String ToJson() const override;

		private:
			std::vector<double> m_buckets;
			std::vector<std::unique_ptr<std::atomic<int64_t>>> m_counts;
			std::atomic<int64_t> m_count{ 0 };
			std::atomic<double> m_sum{ 0 }; // 纳秒累积

			std::atomic<double> m_ema{ 0.0 };
			std::atomic<double> m_min{ std::numeric_limits<double>::infinity() };
			std::atomic<double> m_max{ -std::numeric_limits<double>::infinity() };
			double m_alpha = 0.9;
		};
	}
}