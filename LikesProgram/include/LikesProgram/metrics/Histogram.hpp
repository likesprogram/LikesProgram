#pragma once
#include "../system/LikesProgramLibExport.hpp"
#include "Metrics.hpp"
#include "../time/Timer.hpp"

namespace LikesProgram {
	namespace Metrics {
		class LIKESPROGRAM_API Histogram : public MetricsObject {
		public:
			Histogram(const LikesProgram::String& name, const std::vector<double>& buckets,
				const LikesProgram::String& help = u"",
				const std::map<LikesProgram::String, LikesProgram::String>& labels = {});
			Histogram(const Histogram& other);
			Histogram& operator=(const Histogram& other);
			Histogram(Histogram&& other) noexcept;
			Histogram& operator=(Histogram&& other) noexcept;
			~Histogram() override;

			// value 单位为 秒
			void Observe(double value);
			void ObserveDuration(const LikesProgram::Time::Timer& timer);

			std::vector<double> Buckets() const;

			std::vector<int64_t> Counts() const;

			int64_t Count() const;

			double Sum() const;

			void Reset() override;

			LikesProgram::String Name() const override;
			std::map<LikesProgram::String, LikesProgram::String> Labels() const override;
			LikesProgram::String Help() const override;
			LikesProgram::String Type() const override { return u"histogram"; }
			LikesProgram::String ToPrometheus() const override;
			LikesProgram::String ToJson() const override;

		private:
			struct HistogramImpl;
			HistogramImpl* m_impl;
		};
	}
}