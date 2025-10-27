#pragma once
#include "../system/LikesProgramLibExport.hpp"
#include "Metrics.hpp"

namespace LikesProgram {
	namespace Metrics {
        class LIKESPROGRAM_API Counter : public MetricsObject {
		public:
			Counter(const LikesProgram::String& name, const LikesProgram::String& help = u"",
				const std::map<LikesProgram::String, LikesProgram::String>& labels = {});
            Counter(const Counter& other);
            Counter& operator=(const Counter& other);
			Counter(Counter&& other) noexcept;
            Counter& operator=(Counter&& other) noexcept;
			~Counter() override;

            void Increment(double value = 1.0);
			double Value() const;

			void Reset() override;

			LikesProgram::String Name() const override;
			std::map<LikesProgram::String, LikesProgram::String> Labels() const override;
            LikesProgram::String Help() const override;
			LikesProgram::String Type() const override;
            LikesProgram::String ToPrometheus() const override;
            LikesProgram::String ToJson() const override;
		private:
			struct CounterImpl;
			CounterImpl* m_impl;
        };
	}
}