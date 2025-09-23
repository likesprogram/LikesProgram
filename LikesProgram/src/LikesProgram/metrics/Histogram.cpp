#include "../../../include/LikesProgram/metrics/Histogram.hpp"
#include "../../../include/LikesProgram/math/Math.hpp"
#include <algorithm>
#include <sstream>

namespace LikesProgram {
	namespace Metrics {
        Histogram::Histogram(const LikesProgram::String& name, const std::vector<int64_t>& buckets,
            const LikesProgram::String& help,
            const std::unordered_map<LikesProgram::String, LikesProgram::String>& labels)
            : MetricsObject(name, help, labels) {

            m_buckets = std::vector<int64_t>(buckets.begin(), buckets.end());
            std::sort(m_buckets.begin(), m_buckets.end());

            m_counts.reserve(m_buckets.size());
            for (size_t i = 0; i < m_buckets.size(); ++i) {
                m_counts.emplace_back(std::make_unique<std::atomic<int64_t>>(0));
            }
        }

        void Histogram::Observe(int64_t value) {
            // 更新总数
            m_count.fetch_add(1, std::memory_order_relaxed);
            m_sum.fetch_add(value, std::memory_order_relaxed);

            // 找到合适的桶并增加计数
            for (size_t i = 0; i < m_buckets.size(); ++i) {
                if (value <= m_buckets[i]) {
                    m_counts[i]->fetch_add(1, std::memory_order_relaxed);
                }
            }

            // EMA
            double prev = m_ema.load(std::memory_order_relaxed);
            double next = Math::EMA(prev, value, m_alpha);
            while (!m_ema.compare_exchange_weak(prev, next, std::memory_order_relaxed)) {
                next = Math::EMA(prev, value, m_alpha);
            }

            // Min/Max
            Math::UpdateMin(m_min, value);
            Math::UpdateMax(m_max, value);
        }

        void Histogram::SetEmaAlpha(double alpha) {
            m_alpha = alpha;
        }


        double Histogram::EMA() const {
            return m_ema.load(std::memory_order_relaxed);
        }

        double Histogram::Average() const {
            return Math::Average(m_sum.load(std::memory_order_relaxed),
                m_count.load(std::memory_order_relaxed));
        }

        int64_t Histogram::Max() const {
            return m_max.load(std::memory_order_relaxed);
        }

        int64_t Histogram::Min() const {
            return m_min.load(std::memory_order_relaxed);
        }

        void Histogram::ObserveDuration(const Timer& timer) {
            Observe(timer.GetLastElapsed().count());
        }

        LikesProgram::String Histogram::Name() const {
            return m_name;
        }

        std::unordered_map<LikesProgram::String, LikesProgram::String> Histogram::Labels() const {
            return m_labels;
        }

        LikesProgram::String Histogram::Help() const {
            return m_help;
        }

        LikesProgram::String Histogram::ToPrometheus() const {
            LikesProgram::String result;
            result.Append(u"# HELP ").Append(m_name).Append(u" ").Append(m_help).Append(u" (nanoseconds)\n");
            result.Append(u"# TYPE ").Append(m_name).Append(u" histogram\n");

            LikesProgram::String base = m_name;

            // buckets
            for (size_t i = 0; i < m_buckets.size(); ++i) {
                result.Append(base).Append(u"{le=\"")
                    .Append(LikesProgram::String::FromInt(m_buckets[i]))
                    .Append(u"\"} ")
                    .Append(LikesProgram::String::FromInt(m_counts[i]->load(std::memory_order_relaxed)))
                    .Append(u"\n");
            }

            result.Append(base).Append(u"{le=\"+Inf\"} ")
                .Append(LikesProgram::String::FromInt(m_count.load(std::memory_order_relaxed))).Append(u"\n");

            result.Append(base).Append(u"_sum ")
                .Append(LikesProgram::String::FromInt(m_sum.load(std::memory_order_relaxed))).Append(u"\n");

            result.Append(base).Append(u"_count ")
                .Append(LikesProgram::String::FromInt(m_count.load(std::memory_order_relaxed))).Append(u"\n");

            result.Append(base).Append(u"_ema ")
                .Append(LikesProgram::String::FromFloat(m_ema.load(std::memory_order_relaxed), 6)).Append(u"\n");

            result.Append(base).Append(u"_min ")
                .Append(LikesProgram::String::FromInt(m_min.load(std::memory_order_relaxed))).Append(u"\n");

            result.Append(base).Append(u"_max ")
                .Append(LikesProgram::String::FromInt(m_max.load(std::memory_order_relaxed))).Append(u"\n");

            result.Append(base).Append(u"_avg ").Append(LikesProgram::String::FromFloat(Average())).Append(u"\n");

            return result;
        }


        LikesProgram::String Histogram::ToJson() const {
            LikesProgram::String json;
            json.Append(u"{");
            json.Append(u"\"name\":\"").Append(LikesProgram::String::EscapeJson(m_name)).Append(u"\",");
            json.Append(u"\"help\":\"").Append(LikesProgram::String::EscapeJson(m_help)).Append(u"\",");
            json.Append(u"\"type\":\"").Append(Type()).Append(u"\",");

            // labels
            json.Append(u"\"labels\":{");
            bool first = true;
            for (const auto& [k, v] : m_labels) {
                if (!first) json.Append(u",");
                json.Append(u"\"").Append(LikesProgram::String::EscapeJson(k))
                    .Append(u"\":\"").Append(LikesProgram::String::EscapeJson(v)).Append(u"\"");
                first = false;
            }
            json.Append(u"},");

            // buckets
            json.Append(u"\"buckets\":{");
            for (size_t i = 0; i < m_buckets.size(); ++i) {
                if (i > 0) json.Append(u",");
                json.Append(u"\"").Append(LikesProgram::String::FromInt(m_buckets[i])).Append(u"\":")
                    .Append(LikesProgram::String::FromInt(m_counts[i]->load(std::memory_order_relaxed)));
            }
            json.Append(u"},");

            // stats
            json.Append(u"\"sum\":").Append(LikesProgram::String::FromInt(m_sum.load(std::memory_order_relaxed))).Append(u",");
            json.Append(u"\"count\":").Append(LikesProgram::String::FromInt(m_count.load(std::memory_order_relaxed))).Append(u",");
            json.Append(u"\"ema\":").Append(LikesProgram::String::FromFloat(m_ema.load(std::memory_order_relaxed), 6)).Append(u",");
            json.Append(u"\"min\":").Append(LikesProgram::String::FromInt(m_min.load(std::memory_order_relaxed))).Append(u",");
            json.Append(u"\"max\":").Append(LikesProgram::String::FromInt(m_max.load(std::memory_order_relaxed))).Append(u",");
            json.Append(u"\"average\":").Append(LikesProgram::String::FromFloat(Average()));
            json.Append(u"}");

            return json;
        }
    }
}