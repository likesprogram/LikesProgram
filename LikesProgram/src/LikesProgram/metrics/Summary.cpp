#include "../../../include/LikesProgram/metrics/Summary.hpp"
#include "../../../include/LikesProgram/math/Math.hpp"
#include <cmath>

namespace LikesProgram {
	namespace Metrics {
        struct Summary::SummaryImpl {
            std::atomic<int64_t> m_count{ 0 };
            std::atomic<double> m_sum{ 0.0 };
            std::atomic<double> m_emAverage{ 0.0 };
            std::atomic<double> m_min{ std::numeric_limits<double>::infinity() };
            std::atomic<double> m_max{ -std::numeric_limits<double>::infinity() };
        };
        Summary::Summary(const LikesProgram::String& name,
            size_t maxWindow, const LikesProgram::String& help,
            const std::map<LikesProgram::String, LikesProgram::String>& labels)
            : MetricsObject(name, help, labels), m_maxWindow(maxWindow), m_impl(new SummaryImpl{}) {
        }
        Summary::Summary(const Summary& other) : MetricsObject(other),
        m_maxWindow(other.m_maxWindow), m_alpha(other.m_alpha), m_sketch(other.m_sketch), m_impl(other.m_impl ? new SummaryImpl{} : nullptr) {
            if (other.m_impl) {
                m_impl->m_count.store(other.m_impl->m_count.load(std::memory_order_relaxed), std::memory_order_relaxed);
                m_impl->m_sum.store(other.m_impl->m_sum.load(std::memory_order_relaxed), std::memory_order_relaxed);
                m_impl->m_emAverage.store(other.m_impl->m_emAverage.load(std::memory_order_relaxed), std::memory_order_relaxed);
                m_impl->m_min.store(other.m_impl->m_min.load(std::memory_order_relaxed), std::memory_order_relaxed);
                m_impl->m_max.store(other.m_impl->m_max.load(std::memory_order_relaxed), std::memory_order_relaxed);
            }
        }
        Summary& Summary::operator=(const Summary& other) {
            if (this != &other) {
                MetricsObject::operator=(other);
                m_maxWindow = other.m_maxWindow;
                m_alpha = other.m_alpha;
                m_sketch = other.m_sketch;
                if (other.m_impl) {
                    if (!m_impl) m_impl = new SummaryImpl{};
                    m_impl->m_count.store(other.m_impl->m_count.load(std::memory_order_relaxed), std::memory_order_relaxed);
                    m_impl->m_sum.store(other.m_impl->m_sum.load(std::memory_order_relaxed), std::memory_order_relaxed);
                    m_impl->m_emAverage.store(other.m_impl->m_emAverage.load(std::memory_order_relaxed), std::memory_order_relaxed);
                    m_impl->m_min.store(other.m_impl->m_min.load(std::memory_order_relaxed), std::memory_order_relaxed);
                    m_impl->m_max.store(other.m_impl->m_max.load(std::memory_order_relaxed), std::memory_order_relaxed);
                }
                else {
                    delete m_impl;
                    m_impl = nullptr;
                }
            }
            return *this;
        }
        Summary::Summary(Summary&& other) noexcept : MetricsObject(std::move(other)),
        m_maxWindow(other.m_maxWindow), m_alpha(other.m_alpha), m_sketch(other.m_sketch), m_impl(other.m_impl) {
            other.m_impl = nullptr;
        }
        Summary& Summary::operator=(Summary&& other) noexcept {
            if (this != &other) {
                MetricsObject::operator=(std::move(other));
                m_maxWindow = other.m_maxWindow;
                m_sketch = other.m_sketch;
                m_alpha = other.m_alpha;
                delete m_impl;
                m_impl = other.m_impl;
                other.m_impl = nullptr;
            }
            return *this;
        }
        Summary::~Summary() {
            if (m_impl) delete m_impl;
            m_impl = nullptr;
        }

        void Summary::Observe(double value) {
            UpdateStats(value);

            m_sketch.Add(value);
        }

        double Summary::Quantile(double q) const {
            if (q < 0.0 || q > 1.0) {
                throw std::invalid_argument("Quantile q must be between 0 and 1");
            }
            // 确保 centroids 包含最新数据
            const_cast<Math::PercentileSketch&>(m_sketch).Compress();
            double val = m_sketch.Quantile(q);
            if (std::isnan(val)) return Math::Average(m_impl->m_sum.load(std::memory_order_relaxed),
                m_impl->m_count.load(std::memory_order_relaxed));
            return val;
        }

        int64_t Summary::Count() const {
            return m_impl->m_count.load(std::memory_order_relaxed);
        }

        double Summary::Sum() const {
            return m_impl->m_sum.load(std::memory_order_relaxed);
        }

        void Summary::Reset() {
            m_impl->m_count.store(0, std::memory_order_relaxed);
            m_impl->m_sum.store(0, std::memory_order_relaxed);
            m_impl->m_emAverage.store(0.0, std::memory_order_relaxed);
            m_impl->m_min.store(std::numeric_limits<double>::infinity(), std::memory_order_relaxed);
            m_impl->m_max.store(-std::numeric_limits<double>::infinity(), std::memory_order_relaxed);
            m_sketch.Reset();
        }

        void Summary::SetEMAAlpha(double alpha) {
            m_alpha = alpha;
        }

        double Summary::EMA() const {
            return m_impl->m_emAverage.load(std::memory_order_relaxed);
        }

        double Summary::Min() const {
            return Count() ? m_impl->m_min.load(std::memory_order_relaxed) : std::numeric_limits<double>::max();
        }

        double Summary::Max() const {
            return Count() ? m_impl->m_max.load(std::memory_order_relaxed) : std::numeric_limits<double>::lowest();
        }

        void Summary::UpdateStats(double value) {
            m_impl->m_count.fetch_add(1, std::memory_order_relaxed);
            m_impl->m_sum.fetch_add(value, std::memory_order_relaxed);

            if (m_alpha > 0 && m_alpha < 1) { // EMA
                double prev = m_impl->m_emAverage.load(std::memory_order_relaxed);
                double next = Math::EMA(prev, value, m_alpha);
                while (!m_impl->m_emAverage.compare_exchange_weak(prev, next, std::memory_order_relaxed)) {
                    next = Math::EMA(prev, value, m_alpha);
                }
            }

            // Min / Max
            Math::UpdateMin(m_impl->m_min, value);
            Math::UpdateMax(m_impl->m_max, value);
        }

        LikesProgram::String Summary::Name() const {
            return m_name;
        }

        std::map<LikesProgram::String, LikesProgram::String> Summary::Labels() const {
            return GetLabels();
        }

        LikesProgram::String Summary::Help() const {
            return m_help;
        }

        LikesProgram::String Summary::Type() const {
            return u"summary";
        }

        LikesProgram::String Summary::ToPrometheus() const {
            LikesProgram::String result = u"# HELP ";
            result.Append(m_name).Append(u" ").Append(m_help).Append(u"\n");
            result.Append(u"# TYPE ").Append(m_name).Append(u" ");
            result.Append(Type()).Append(u"\n");

            auto appendMetric = [&](const LikesProgram::String& suffix, const LikesProgram::String& value) {
                result.Append(m_name);
                if (!suffix.Empty()) result.Append(suffix);
                if (!GetLabels().empty()) {
                    result.Append(u"{");
                    bool first = true;
                    for (const auto& [k, v] : GetLabels()) {
                        if (!first) result.Append(u",");
                        result.Append(k).Append(u"=\"").Append(v).Append(u"\"");
                        first = false;
                    }
                    result.Append(u"}");
                }
                result.Append(u" ").Append(value).Append(u"\n");
            };

            auto appendQuantile = [&](double q, double val) {
                result.Append(m_name);
                if (!GetLabels().empty() || true) { // 强制有 quantile 标签
                    result.Append(u"{");
                    bool first = true;
                    for (const auto& [k, v] : GetLabels()) {
                        if (!first) result.Append(u",");
                        result.Append(k).Append(u"=\"").Append(v).Append(u"\"");
                        first = false;
                    }
                    if (!first) result.Append(u",");
                    result.Append(u"quantile=\"").Append(LikesProgram::String::Format(u"{:.2f}", q)).Append(u"\"");
                    result.Append(u"}");
                }
                result.Append(u" ").Append(LikesProgram::String::Format(u"{:.6f}", val)).Append(u"\n");
            };
            appendMetric(u"_count", LikesProgram::String::Format(u"{}", m_impl->m_count.load(std::memory_order_relaxed)));
            appendMetric(u"_sum", LikesProgram::String::Format(u"{:.6f}", m_impl->m_sum.load(std::memory_order_relaxed)));

            appendQuantile(0.5, Quantile(0.5));
            appendQuantile(0.9, Quantile(0.9));
            appendQuantile(0.99, Quantile(0.99));

            // 扩展字段
            if (Count() > 0) {
                if (m_alpha > 0 && m_alpha < 1) appendMetric(u"_ema", LikesProgram::String::Format(u"{:.6f}", m_impl->m_emAverage.load(std::memory_order_relaxed)));
                appendMetric(u"_min", LikesProgram::String::Format(u"{:.6f}", Min()));
                appendMetric(u"_max", LikesProgram::String::Format(u"{:.6f}", Max()));
            }

            return result;
        }

        LikesProgram::String Summary::ToJson() const {
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

            json.Append(u"\"count\":").Append(LikesProgram::String::Format(u"{}", m_impl->m_count.load(std::memory_order_relaxed))).Append(u",");
            json.Append(u"\"sum\":").Append(LikesProgram::String::Format(u"{:.6f}", m_impl->m_sum.load(std::memory_order_relaxed))).Append(u",");

            json.Append(u"\"0.50\":").Append(LikesProgram::String::Format(u"{:.6f}", Quantile(0.5))).Append(u",");
            json.Append(u"\"0.90\":").Append(LikesProgram::String::Format(u"{:.6f}", Quantile(0.9))).Append(u",");
            json.Append(u"\"0.99\":").Append(LikesProgram::String::Format(u"{:.6f}", Quantile(0.99)));

            // 扩展字段
            if (Count() > 0) {
                std::vector<LikesProgram::String> fields;
                if (m_alpha > 0 && m_alpha < 1) fields.push_back(u"\"ema\":" + LikesProgram::String::Format(u"{:.6f}", m_impl->m_emAverage.load(std::memory_order_relaxed)));
                fields.push_back(u"\"min\":" + LikesProgram::String::Format(u"{:.6f}", Min()));
                fields.push_back(u"\"max\":" + LikesProgram::String::Format(u"{:.6f}", Max()));
                for (size_t i = 0; i < fields.size(); ++i) {
                    json.Append(u",");
                    json.Append(fields[i]);
                }
            }

            json.Append(u"}");

            return json;
        }
	}
}