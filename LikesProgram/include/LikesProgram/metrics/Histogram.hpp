#pragma once
#include "../LikesProgramLibExport.hpp"
#include "Metrics.hpp"
#include "../Timer.hpp"

namespace LikesProgram {
	namespace Metrics {
		class LIKESPROGRAM_API Histogram : public MetricsObject {
		public:
			Histogram(const LikesProgram::String& name, const std::vector<int64_t>& buckets,
				const LikesProgram::String& help = u"",
				const std::unordered_map<LikesProgram::String, LikesProgram::String>& labels = {});

			// value 单位为 纳秒
			void Observe(int64_t value);
			void ObserveDuration(const Timer& timer);

			void SetEmaAlpha(double alpha);

			double EMA() const;
			double Average() const;

			int64_t Max() const;
			int64_t Min() const;

			LikesProgram::String Name() const override;
			std::unordered_map<LikesProgram::String, LikesProgram::String> Labels() const override;
			LikesProgram::String Help() const override;
			LikesProgram::String Type() const override { return u"histogram"; }
			LikesProgram::String ToPrometheus() const override;
			LikesProgram::String ToJson() const override;

		private:
			std::vector<int64_t> m_buckets;
			std::vector<std::unique_ptr<std::atomic<int64_t>>> m_counts;
			std::atomic<int64_t> m_count{ 0 };
			std::atomic<int64_t> m_sum{ 0 }; // 纳秒累积

			std::atomic<double> m_ema{ 0.0 };
			std::atomic<int64_t> m_max{ std::numeric_limits<int64_t>::max() };
			std::atomic<int64_t> m_min{ std::numeric_limits<int64_t>::min() };
			double m_alpha = 0.9;
		};
	}
}