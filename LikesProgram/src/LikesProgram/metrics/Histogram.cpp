#include "../../../include/LikesProgram/metrics/Histogram.hpp"
#include "../../../include/LikesProgram/math/Math.hpp"
#include "../../../include/LikesProgram/time/Time.hpp"
#include <algorithm>
#include <sstream>

namespace LikesProgram {
	namespace Metrics {
        struct Histogram::HistogramImpl {
            std::vector<double> m_buckets;
            std::vector<std::unique_ptr<std::atomic<int64_t>>> m_counts;
            std::atomic<int64_t> m_count{ 0 };
            std::atomic<double> m_sum{ 0 }; // 秒累积
        };

        Histogram::Histogram(const LikesProgram::String& name, const std::vector<double>& buckets,
            const LikesProgram::String& help,
            const std::map<LikesProgram::String, LikesProgram::String>& labels)
            : MetricsObject(name, help, labels), m_impl(new HistogramImpl{}) {

            m_impl->m_buckets = std::vector<double>(buckets.begin(), buckets.end());
            std::sort(m_impl->m_buckets.begin(), m_impl->m_buckets.end());

            m_impl->m_counts.reserve(m_impl->m_buckets.size());
            for (size_t i = 0; i < m_impl->m_buckets.size(); ++i) {
                m_impl->m_counts.emplace_back(std::make_unique<std::atomic<int64_t>>(0));
            }
        }
        Histogram::Histogram(const Histogram& other) : MetricsObject(other), m_impl(other.m_impl ? new HistogramImpl{} : nullptr) {
            if (other.m_impl) {
                m_impl->m_buckets = other.m_impl->m_buckets;

                m_impl->m_counts.reserve(other.m_impl->m_counts.size());
                for (const auto& cptr : other.m_impl->m_counts) {
                    m_impl->m_counts.emplace_back(std::make_unique<std::atomic<int64_t>>(cptr->load(std::memory_order_relaxed)));
                }

                m_impl->m_count.store(other.m_impl->m_count.load(std::memory_order_relaxed), std::memory_order_relaxed);
                m_impl->m_sum.store(other.m_impl->m_sum.load(std::memory_order_relaxed), std::memory_order_relaxed);
            }
        }
        Histogram& Histogram::operator=(const Histogram& other) {
            if (this != &other) {
                MetricsObject::operator=(other);
                if (other.m_impl) {
                    if (!m_impl) m_impl = new HistogramImpl{};
                    m_impl->m_buckets = other.m_impl->m_buckets;

                    m_impl->m_counts.clear();
                    m_impl->m_counts.reserve(other.m_impl->m_counts.size());
                    for (const auto& cptr : other.m_impl->m_counts) {
                        m_impl->m_counts.emplace_back(std::make_unique<std::atomic<int64_t>>(cptr->load(std::memory_order_relaxed)));
                    }

                    m_impl->m_count.store(other.m_impl->m_count.load(std::memory_order_relaxed), std::memory_order_relaxed);
                    m_impl->m_sum.store(other.m_impl->m_sum.load(std::memory_order_relaxed), std::memory_order_relaxed);
                }
                else {
                    delete m_impl;
                    m_impl = nullptr;
                }
            }
            return *this;
        }
        Histogram::Histogram(Histogram&& other) noexcept : MetricsObject(std::move(other)), m_impl(other.m_impl) {
            other.m_impl = nullptr;
        }
        Histogram& Histogram::operator=(Histogram&& other) noexcept {
            if (this != &other) {
                MetricsObject::operator=(std::move(other));
                delete m_impl;
                m_impl = other.m_impl;
                other.m_impl = nullptr;
            }
            return *this;
        }
        Histogram::~Histogram() {
            if (m_impl) delete m_impl;
            m_impl = nullptr;
        }

        void Histogram::Observe(double value) {
            // 更新总数
            m_impl->m_count.fetch_add(1, std::memory_order_relaxed);
            m_impl->m_sum.fetch_add(value, std::memory_order_relaxed);

            // 找到合适的桶并增加计数
            for (size_t i = 0; i < m_impl->m_buckets.size(); ++i) {
                if (value <= m_impl->m_buckets[i]) {
                    m_impl->m_counts[i]->fetch_add(1, std::memory_order_relaxed);
                }
            }
        }

        void Histogram::ObserveDuration(const LikesProgram::Time::Timer& timer) {
            double value_s = LikesProgram::Time::NsToS(timer.GetLastElapsed().count());
            Observe(value_s);
        }

        std::vector<double> Histogram::Buckets() const {
            return m_impl->m_buckets;
        }

        std::vector<int64_t> Histogram::Counts() const {
            std::vector<int64_t> result;

            result.reserve(m_impl->m_counts.size());
            for (const auto& c : m_impl->m_counts) {
                result.push_back(c->load(std::memory_order_relaxed));
            }

            return result;
        }

        int64_t Histogram::Count() const {
            return m_impl->m_count.load(std::memory_order_relaxed);
        }

        double Histogram::Sum() const {
            return m_impl->m_sum.load(std::memory_order_relaxed);
        }

        void Histogram::Reset() {
            for (auto& cptr : m_impl->m_counts) {
                cptr->store(0, std::memory_order_relaxed);
            }

            m_impl->m_count.store(0, std::memory_order_relaxed);
            m_impl->m_sum.store(0.0, std::memory_order_relaxed);
        }

        LikesProgram::String Histogram::Name() const {
            return m_name;
        }

        std::map<LikesProgram::String, LikesProgram::String> Histogram::Labels() const {
            return GetLabels();
        }

        LikesProgram::String Histogram::Help() const {
            return m_help;
        }

        LikesProgram::String Histogram::ToPrometheus() const {
            LikesProgram::String result = u"# HELP ";
            result.Append(m_name).Append(u" ").Append(m_help).Append(u"\n");
            result.Append(u"# TYPE ").Append(m_name).Append(u" ");
            result.Append(Type()).Append(u"\n");

            LikesProgram::String base = m_name;
            for (size_t i = 0; i < m_impl->m_buckets.size(); ++i) {
                result.Append(base).Append(u"{le=\"")
                    .Append(LikesProgram::String::FromFloat(m_impl->m_buckets[i], 6))
                    .Append(u"\"} ")
                    .Append(LikesProgram::String::FromInt(m_impl->m_counts[i]->load(std::memory_order_relaxed)))
                    .Append(u"\n");
            }

            result.Append(base).Append(u"{le=\"+Inf\"} ")
                .Append(LikesProgram::String::FromInt(m_impl->m_count.load(std::memory_order_relaxed))).Append(u"\n");

            result.Append(base).Append(u"_sum ")
                .Append(LikesProgram::String::FromFloat(m_impl->m_sum.load(std::memory_order_relaxed), 6)).Append(u"\n");

            result.Append(base).Append(u"_count ")
                .Append(LikesProgram::String::FromInt(m_impl->m_count.load(std::memory_order_relaxed))).Append(u"\n");

            return result;
        }


        LikesProgram::String Histogram::ToJson() const {
            LikesProgram::String json;
            json.Append(u"{");
            json.Append(u"\"name\":\"").Append(LikesProgram::String::EscapeJson(m_name)).Append(u"\",");
            json.Append(u"\"help\":\"").Append(LikesProgram::String::EscapeJson(m_help)).Append(u"\",");
            json.Append(u"\"type\":\"").Append(Type()).Append(u"\",");

            json.Append(u"\"labels\":{");
            bool first = true;
            for (const auto& [k, v] : GetLabels()) {
                if (!first) json.Append(u",");
                json.Append(u"\"").Append(LikesProgram::String::EscapeJson(k))
                    .Append(u"\":\"").Append(LikesProgram::String::EscapeJson(v)).Append(u"\"");
                first = false;
            }
            json.Append(u"},");

            json.Append(u"\"buckets\":{");
            for (size_t i = 0; i < m_impl->m_buckets.size(); ++i) {
                if (i > 0) json.Append(u",");
                json.Append(u"\"").Append(LikesProgram::String::FromFloat(m_impl->m_buckets[i], 6)).Append(u"\":")
                    .Append(LikesProgram::String::FromInt(m_impl->m_counts[i]->load(std::memory_order_relaxed)));
            }
            json.Append(u"},");

            json.Append(u"\"sum\":").Append(LikesProgram::String::FromFloat(m_impl->m_sum.load(std::memory_order_relaxed), 6)).Append(u",");
            json.Append(u"\"count\":").Append(LikesProgram::String::FromInt(m_impl->m_count.load(std::memory_order_relaxed)));
            json.Append(u"}");

            return json;
        }
    }
}