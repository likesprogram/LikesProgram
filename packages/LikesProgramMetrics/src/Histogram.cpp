#include <LikesProgram/Metrics/Histogram.hpp>
#include <metrics/MetricsInternal.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>

namespace LikesProgram {
    namespace Metrics {
        namespace {
            // 复制并规范化桶边界：丢弃 NaN，排序后去重。
            std::vector<double> NormalizeBuckets(const std::vector<double>& buckets) {
                std::vector<double> normalized;       // 可导出的有序桶边界
                normalized.reserve(buckets.size());

                for (double bucket : buckets) {
                    if (std::isfinite(bucket)) normalized.push_back(bucket);
                }

                std::sort(normalized.begin(), normalized.end());
                normalized.erase(std::unique(normalized.begin(), normalized.end()), normalized.end());
                return normalized;
            }

            // 合并基础标签和当前样本的额外标签。
            std::map<LikesProgram::String, LikesProgram::String> WithExtraLabel(
                std::map<LikesProgram::String, LikesProgram::String> labels,
                const LikesProgram::String& key,
                const LikesProgram::String& value) {
                labels[key] = value;
                return labels;
            }
        }

        struct Histogram::HistogramImpl {
            std::vector<double> m_buckets;                                  // 已排序桶边界
            std::vector<std::unique_ptr<std::atomic<int64_t>>> m_counts;     // 每个有限桶的命中计数
            std::atomic<int64_t> m_count{ 0 };                               // 总样本数
            std::atomic<double> m_sum{ 0.0 };                                // 样本值总和
        };

        Histogram::Histogram(const LikesProgram::String& name, const std::vector<double>& buckets,
            const LikesProgram::String& help,
            const std::map<LikesProgram::String, LikesProgram::String>& labels)
            : Metric(name, help, labels), m_impl(new HistogramImpl{}) {
            // 桶边界固定在构造阶段，后续只更新原子计数。
            m_impl->m_buckets = NormalizeBuckets(buckets);
            m_impl->m_counts.reserve(m_impl->m_buckets.size());
            for (size_t i = 0; i < m_impl->m_buckets.size(); ++i) {
                m_impl->m_counts.emplace_back(std::make_unique<std::atomic<int64_t>>(0));
            }
        }

        Histogram::Histogram(const Histogram& other)
            : Metric(other), m_impl(new HistogramImpl{}) {
            if (!other.m_impl) return;

            // 拷贝桶结构，再按桶复制当前计数快照。
            m_impl->m_buckets = other.m_impl->m_buckets;
            m_impl->m_counts.reserve(other.m_impl->m_counts.size());
            for (const auto& count : other.m_impl->m_counts) {
                m_impl->m_counts.emplace_back(
                    std::make_unique<std::atomic<int64_t>>(count->load(std::memory_order_relaxed)));
            }
            m_impl->m_count.store(other.m_impl->m_count.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
            m_impl->m_sum.store(other.m_impl->m_sum.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
        }

        Histogram& Histogram::operator=(const Histogram& other) {
            if (this == &other) return *this;

            Metric::operator=(other);
            if (!m_impl) m_impl = new HistogramImpl{};
            m_impl->m_buckets.clear();
            m_impl->m_counts.clear();

            if (other.m_impl) {
                // 重新构造计数数组，避免复用旧桶数量导致边界错位。
                m_impl->m_buckets = other.m_impl->m_buckets;
                m_impl->m_counts.reserve(other.m_impl->m_counts.size());
                for (const auto& count : other.m_impl->m_counts) {
                    m_impl->m_counts.emplace_back(
                        std::make_unique<std::atomic<int64_t>>(count->load(std::memory_order_relaxed)));
                }
                m_impl->m_count.store(other.m_impl->m_count.load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
                m_impl->m_sum.store(other.m_impl->m_sum.load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
            }
            else {
                m_impl->m_count.store(0, std::memory_order_relaxed);
                m_impl->m_sum.store(0.0, std::memory_order_relaxed);
            }
            return *this;
        }

        Histogram::Histogram(Histogram&& other) noexcept
            : Metric(std::move(other)), m_impl(other.m_impl) {
            other.m_impl = nullptr;
        }

        Histogram& Histogram::operator=(Histogram&& other) noexcept {
            if (this == &other) return *this;

            Metric::operator=(std::move(other));
            delete m_impl;
            m_impl = other.m_impl;
            other.m_impl = nullptr;
            return *this;
        }

        Histogram::~Histogram() {
            delete m_impl;
            m_impl = nullptr;
        }

        void Histogram::Observe(double value) {
            // 非有限值不导出，避免污染 sum 和桶计数。
            if (!m_impl || !std::isfinite(value)) return;

            Internal::IncrementSaturating(m_impl->m_count);
            Internal::AddFiniteSaturating(m_impl->m_sum, value);

            // 热路径只更新命中的第一个有限桶，导出时再转为 Prometheus 累计桶。
            auto iter = std::lower_bound(m_impl->m_buckets.begin(), m_impl->m_buckets.end(), value);
            if (iter != m_impl->m_buckets.end()) {
                const size_t index = static_cast<size_t>(iter - m_impl->m_buckets.begin()); // 命中桶索引
                Internal::IncrementSaturating(*m_impl->m_counts[index]);
            }
        }

        void Histogram::ObserveDuration(const LikesProgram::Time::Timer& timer) {
            // Timer 返回纳秒 Duration，Histogram 统一按秒记录延迟。
            const double seconds = LikesProgram::Time::NsToS(timer.GetLastElapsed().count());
            Observe(seconds);
        }

        std::vector<double> Histogram::Buckets() const {
            if (!m_impl) return {};
            return m_impl->m_buckets;
        }

        std::vector<int64_t> Histogram::Counts() const {
            std::vector<int64_t> result; // 当前桶计数快照
            if (!m_impl) return result;

            result.reserve(m_impl->m_counts.size());
            int64_t cumulative = 0; // 返回值保持旧 API 的累计桶语义
            for (const auto& count : m_impl->m_counts) {
                cumulative = Internal::AddInt64Saturating(
                    cumulative,
                    count->load(std::memory_order_relaxed));
                result.push_back(cumulative);
            }
            return result;
        }

        int64_t Histogram::Count() const {
            if (!m_impl) return 0;
            return m_impl->m_count.load(std::memory_order_relaxed);
        }

        double Histogram::Sum() const {
            if (!m_impl) return 0.0;
            return Internal::FiniteForExport(m_impl->m_sum.load(std::memory_order_relaxed));
        }

        void Histogram::Reset() {
            if (!m_impl) return;

            for (auto& count : m_impl->m_counts) {
                count->store(0, std::memory_order_relaxed);
            }
            m_impl->m_count.store(0, std::memory_order_relaxed);
            m_impl->m_sum.store(0.0, std::memory_order_relaxed);
        }

        LikesProgram::String Histogram::Name() const {
            return m_name;
        }

        std::map<LikesProgram::String, LikesProgram::String> Histogram::Labels() const {
            return LabelsCopy();
        }

        LikesProgram::String Histogram::Help() const {
            return m_help;
        }

        LikesProgram::String Histogram::Type() const {
            return u"histogram";
        }

        LikesProgram::String Histogram::ToPrometheus() const {
            const auto labels = LabelsCopy();          // 本次导出的基础标签快照
            LikesProgram::String result = u"# HELP ";

            result.Append(m_name).Append(u" ").Append(m_help).Append(u"\n");
            result.Append(u"# TYPE ").Append(m_name).Append(u" ").Append(Type()).Append(u"\n");

            if (m_impl) {
                int64_t cumulative = 0;                 // Prometheus bucket 必须是累计计数
                for (size_t i = 0; i < m_impl->m_buckets.size(); ++i) {
                    cumulative = Internal::AddInt64Saturating(
                        cumulative,
                        m_impl->m_counts[i]->load(std::memory_order_relaxed));
                    const auto bucketLabels = WithExtraLabel(labels, u"le",
                        LikesProgram::String::Format(u"{:.6f}", m_impl->m_buckets[i]));
                    result.Append(m_name).Append(u"_bucket")
                        .Append(FormatLabels(bucketLabels)).Append(u" ")
                        .Append(LikesProgram::String::Format(u"{}", cumulative))
                        .Append(u"\n");
                }
            }

            const auto infLabels = WithExtraLabel(labels, u"le", u"+Inf");
            result.Append(m_name).Append(u"_bucket").Append(FormatLabels(infLabels)).Append(u" ")
                .Append(LikesProgram::String::Format(u"{}", Count())).Append(u"\n");
            result.Append(m_name).Append(u"_sum").Append(FormatLabels(labels)).Append(u" ")
                .Append(LikesProgram::String::Format(u"{:.6f}", Sum())).Append(u"\n");
            result.Append(m_name).Append(u"_count").Append(FormatLabels(labels)).Append(u" ")
                .Append(LikesProgram::String::Format(u"{}", Count())).Append(u"\n");
            return result;
        }

        LikesProgram::String Histogram::ToJson() const {
            const auto labels = LabelsCopy();          // JSON 导出标签快照
            LikesProgram::String json = u"{";

            json.Append(u"\"name\":\"").Append(LikesProgram::String::EscapeJson(m_name)).Append(u"\",");
            json.Append(u"\"help\":\"").Append(LikesProgram::String::EscapeJson(m_help)).Append(u"\",");
            json.Append(u"\"type\":\"").Append(Type()).Append(u"\",");
            json.Append(u"\"labels\":{");

            bool first = true;                         // label 对象逗号控制
            for (const auto& [key, value] : labels) {
                if (!first) json.Append(u",");
                json.Append(u"\"").Append(LikesProgram::String::EscapeJson(key))
                    .Append(u"\":\"").Append(LikesProgram::String::EscapeJson(value)).Append(u"\"");
                first = false;
            }
            json.Append(u"},\"buckets\":{");

            if (m_impl) {
                int64_t cumulative = 0;                 // JSON 与 Counts 一样保持累计桶语义
                for (size_t i = 0; i < m_impl->m_buckets.size(); ++i) {
                    cumulative = Internal::AddInt64Saturating(
                        cumulative,
                        m_impl->m_counts[i]->load(std::memory_order_relaxed));
                    if (i > 0) json.Append(u",");
                    json.Append(u"\"").Append(LikesProgram::String::Format(u"{:.6f}",
                        m_impl->m_buckets[i])).Append(u"\":")
                        .Append(LikesProgram::String::Format(u"{}", cumulative));
                }
            }

            json.Append(u"},\"sum\":").Append(LikesProgram::String::Format(u"{:.6f}", Sum()))
                .Append(u",\"count\":").Append(LikesProgram::String::Format(u"{}", Count()))
                .Append(u"}");
            return json;
        }
    }
}
