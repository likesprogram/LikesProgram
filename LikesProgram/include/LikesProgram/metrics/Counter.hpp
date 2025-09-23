#pragma once
#include "../LikesProgramLibExport.hpp"
#include "Metrics.hpp"

namespace LikesProgram {
	namespace Metrics {
        class LIKESPROGRAM_API Counter : public MetricsObject {
		public:
			Counter(const LikesProgram::String& name, const LikesProgram::String& help = u"",
				const std::unordered_map<LikesProgram::String, LikesProgram::String>& labels = {});

            void Increment(int64_t value = 1.0);
			int64_t Value() const;

			LikesProgram::String Name() const override;
			std::unordered_map<LikesProgram::String, LikesProgram::String> Labels() const override;
            LikesProgram::String Help() const override;
			LikesProgram::String Type() const override;
            LikesProgram::String ToPrometheus() const override;
            LikesProgram::String ToJson() const override;
		private:
			std::atomic<int64_t> m_value{ 0 };
        };
	}
}