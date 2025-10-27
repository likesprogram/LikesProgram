#pragma once
#include "../system/LikesProgramLibExport.hpp"
#include "Metrics.hpp"

namespace LikesProgram {
	namespace Metrics {
        class LIKESPROGRAM_API Gauge : public MetricsObject {
        public:
            Gauge(const LikesProgram::String& name, const LikesProgram::String& help = u"",
                const std::map<LikesProgram::String, LikesProgram::String>& labels = {});
            Gauge(const Gauge& other);
            Gauge& operator=(const Gauge& other);
            Gauge(Gauge&& other) noexcept;
            Gauge& operator=(Gauge&& other) noexcept;
            ~Gauge() override;
            
            void Set(double value);

            void Increment(double value = 1.0);
            void Decrement(double value = 1.0);
            double Value() const;

            void Reset() override;

            LikesProgram::String Name() const override;
            std::map<LikesProgram::String, LikesProgram::String> Labels() const override;
            LikesProgram::String Help() const override;
            LikesProgram::String Type() const override;
            LikesProgram::String ToPrometheus() const override;
            LikesProgram::String ToJson() const override;

        private:
            struct GaugeImpl;
            GaugeImpl* m_impl;
        };
	}
}