#include <LikesProgram/Metrics/Summary.hpp>
#include <metrics/MetricsInternal.hpp>
#include <metrics/PercentileSketch.hpp>

#include <atomic>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace LikesProgram {
    namespace Metrics {
        namespace {
            // 合并基础标签和 summary 分位标签。
            std::map<LikesProgram::String, LikesProgram::String> WithQuantileLabel(
                std::map<LikesProgram::String, LikesProgram::String> labels,
                double quantile) {
                labels[u"quantile"] = LikesProgram::String::Format(u"{:.2f}", quantile);
                return labels;
            }

            // 计算指数移动平均的新值。
            double ComputeEma(double previous, double value, double alpha) {
                return alpha * value + (1.0 - alpha) * previous;
            }

            // 原子最小值更新，失败时使用 CAS 返回的新快照继续判断。
            void UpdateAtomicMin(std::atomic<double>& target, double value) {
                double current = target.load(std::memory_order_relaxed); // 当前最小值快照
                while (value < current
                    && !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
                }
            }

            // 原子最大值更新，失败时使用 CAS 返回的新快照继续判断。
            void UpdateAtomicMax(std::atomic<double>& target, double value) {
                double current = target.load(std::memory_order_relaxed); // 当前最大值快照
                while (value > current
                    && !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
                }
            }
        }

        struct Summary::SummaryImpl {
            size_t m_maxWindow = 1000;                    // sketch 压缩窗口规模
            std::atomic<double> m_alpha{ -1.0 };           // EMA alpha，非法值表示关闭
            Internal::PercentileSketch m_sketch;           // 包内私有百分位估算器
            std::atomic<int64_t> m_count{ 0 };             // 总样本数
            std::atomic<double> m_sum{ 0.0 };              // 样本值总和
            std::atomic<double> m_emAverage{ 0.0 };        // 当前指数移动平均
            std::atomic<double> m_min{ std::numeric_limits<double>::infinity() }; // 当前最小值
            std::atomic<double> m_max{ -std::numeric_limits<double>::infinity() }; // 当前最大值

            // 按窗口规模初始化 sketch，shards 固定为轻量默认值。
            explicit SummaryImpl(size_t maxWindow)
                : m_maxWindow(maxWindow == 0 ? 1 : maxWindow),
                m_sketch(m_maxWindow, 8) {
            }
        };

        Summary::Summary(const LikesProgram::String& name, size_t maxWindow,
            const LikesProgram::String& help,
            const std::map<LikesProgram::String, LikesProgram::String>& labels)
            : Metric(name, help, labels), m_impl(new SummaryImpl(maxWindow)) {
        }

        Summary::Summary(const Summary& other)
            : Metric(other),
            m_impl(new SummaryImpl(other.m_impl ? other.m_impl->m_maxWindow : 1000)) {
            if (!other.m_impl) return;

            // sketch 需要深拷贝，统计原子值按 relaxed 快照复制。
            m_impl->m_sketch = other.m_impl->m_sketch;
            m_impl->m_alpha.store(other.m_impl->m_alpha.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
            m_impl->m_count.store(other.m_impl->m_count.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
            m_impl->m_sum.store(other.m_impl->m_sum.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
            m_impl->m_emAverage.store(other.m_impl->m_emAverage.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
            m_impl->m_min.store(other.m_impl->m_min.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
            m_impl->m_max.store(other.m_impl->m_max.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
        }

        Summary& Summary::operator=(const Summary& other) {
            if (this == &other) return *this;

            Metric::operator=(other);
            SummaryImpl* next = new SummaryImpl(other.m_impl ? other.m_impl->m_maxWindow : 1000);
            if (other.m_impl) {
                // 构造新实现再替换，保证复制过程中异常不会破坏目标对象。
                next->m_sketch = other.m_impl->m_sketch;
                next->m_alpha.store(other.m_impl->m_alpha.load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
                next->m_count.store(other.m_impl->m_count.load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
                next->m_sum.store(other.m_impl->m_sum.load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
                next->m_emAverage.store(other.m_impl->m_emAverage.load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
                next->m_min.store(other.m_impl->m_min.load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
                next->m_max.store(other.m_impl->m_max.load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
            }

            delete m_impl;
            m_impl = next;
            return *this;
        }

        Summary::Summary(Summary&& other) noexcept
            : Metric(std::move(other)), m_impl(other.m_impl) {
            other.m_impl = nullptr;
        }

        Summary& Summary::operator=(Summary&& other) noexcept {
            if (this == &other) return *this;

            Metric::operator=(std::move(other));
            delete m_impl;
            m_impl = other.m_impl;
            other.m_impl = nullptr;
            return *this;
        }

        Summary::~Summary() {
            delete m_impl;
            m_impl = nullptr;
        }

        void Summary::Observe(double value) {
            // 非有限值会破坏导出和分位估算，直接忽略。
            if (!m_impl || !std::isfinite(value)) return;
            UpdateStats(value);
            m_impl->m_sketch.Add(value);
        }

        double Summary::Quantile(double q) const {
            if (q < 0.0 || q > 1.0) {
                throw std::invalid_argument("Quantile q must be between 0 and 1");
            }
            if (!m_impl) return 0.0;

            m_impl->m_sketch.Compress();
            const double value = m_impl->m_sketch.Quantile(q);
            if (!std::isnan(value)) return value;

            const int64_t count = Count(); // 空 sketch 时回退到均值或 0
            return count > 0 ? Sum() / static_cast<double>(count) : 0.0;
        }

        int64_t Summary::Count() const {
            if (!m_impl) return 0;
            return m_impl->m_count.load(std::memory_order_relaxed);
        }

        double Summary::Sum() const {
            if (!m_impl) return 0.0;
            return Internal::FiniteForExport(m_impl->m_sum.load(std::memory_order_relaxed));
        }

        void Summary::Reset() {
            if (!m_impl) return;

            m_impl->m_count.store(0, std::memory_order_relaxed);
            m_impl->m_sum.store(0.0, std::memory_order_relaxed);
            m_impl->m_emAverage.store(0.0, std::memory_order_relaxed);
            m_impl->m_min.store(std::numeric_limits<double>::infinity(), std::memory_order_relaxed);
            m_impl->m_max.store(-std::numeric_limits<double>::infinity(), std::memory_order_relaxed);
            m_impl->m_sketch.Reset();
        }

        void Summary::SetEMAAlpha(double alpha) {
            if (!m_impl) m_impl = new SummaryImpl(1000);
            m_impl->m_alpha.store(alpha, std::memory_order_relaxed);
        }

        double Summary::EMA() const {
            if (!m_impl) return 0.0;
            return m_impl->m_emAverage.load(std::memory_order_relaxed);
        }

        double Summary::Min() const {
            if (Count() == 0) return std::numeric_limits<double>::max();
            return m_impl->m_min.load(std::memory_order_relaxed);
        }

        double Summary::Max() const {
            if (Count() == 0) return std::numeric_limits<double>::lowest();
            return m_impl->m_max.load(std::memory_order_relaxed);
        }

        void Summary::UpdateStats(double value) {
            Internal::IncrementSaturating(m_impl->m_count);
            Internal::AddFiniteSaturating(m_impl->m_sum, value);
            UpdateAtomicMin(m_impl->m_min, value);
            UpdateAtomicMax(m_impl->m_max, value);

            const double alpha = m_impl->m_alpha.load(std::memory_order_relaxed);
            if (alpha > 0.0 && alpha < 1.0) {
                double previous = m_impl->m_emAverage.load(std::memory_order_relaxed); // EMA 快照
                double next = ComputeEma(previous, value, alpha);
                while (!m_impl->m_emAverage.compare_exchange_weak(previous, next,
                    std::memory_order_relaxed)) {
                    next = ComputeEma(previous, value, alpha);
                }
            }
        }

        LikesProgram::String Summary::Name() const {
            return m_name;
        }

        std::map<LikesProgram::String, LikesProgram::String> Summary::Labels() const {
            return LabelsCopy();
        }

        LikesProgram::String Summary::Help() const {
            return m_help;
        }

        LikesProgram::String Summary::Type() const {
            return u"summary";
        }

        LikesProgram::String Summary::ToPrometheus() const {
            const auto labels = LabelsCopy();          // 本次导出的基础标签快照
            LikesProgram::String result = u"# HELP ";

            result.Append(m_name).Append(u" ").Append(m_help).Append(u"\n");
            result.Append(u"# TYPE ").Append(m_name).Append(u" ").Append(Type()).Append(u"\n");
            result.Append(m_name).Append(u"_count").Append(FormatLabels(labels)).Append(u" ")
                .Append(LikesProgram::String::Format(u"{}", Count())).Append(u"\n");
            result.Append(m_name).Append(u"_sum").Append(FormatLabels(labels)).Append(u" ")
                .Append(LikesProgram::String::Format(u"{:.6f}", Sum())).Append(u"\n");

            for (double quantile : { 0.5, 0.9, 0.99 }) {
                const auto quantileLabels = WithQuantileLabel(labels, quantile);
                result.Append(m_name).Append(FormatLabels(quantileLabels)).Append(u" ")
                    .Append(LikesProgram::String::Format(u"{:.6f}", Quantile(quantile)))
                    .Append(u"\n");
            }

            if (Count() > 0) {
                const double alpha = m_impl ? m_impl->m_alpha.load(std::memory_order_relaxed) : -1.0;
                if (alpha > 0.0 && alpha < 1.0) {
                    result.Append(m_name).Append(u"_ema").Append(FormatLabels(labels)).Append(u" ")
                        .Append(LikesProgram::String::Format(u"{:.6f}", EMA())).Append(u"\n");
                }
                result.Append(m_name).Append(u"_min").Append(FormatLabels(labels)).Append(u" ")
                    .Append(LikesProgram::String::Format(u"{:.6f}", Min())).Append(u"\n");
                result.Append(m_name).Append(u"_max").Append(FormatLabels(labels)).Append(u" ")
                    .Append(LikesProgram::String::Format(u"{:.6f}", Max())).Append(u"\n");
            }

            return result;
        }

        LikesProgram::String Summary::ToJson() const {
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

            json.Append(u"},\"count\":").Append(LikesProgram::String::Format(u"{}", Count()));
            json.Append(u",\"sum\":").Append(LikesProgram::String::Format(u"{:.6f}", Sum()));
            json.Append(u",\"0.50\":").Append(LikesProgram::String::Format(u"{:.6f}", Quantile(0.5)));
            json.Append(u",\"0.90\":").Append(LikesProgram::String::Format(u"{:.6f}", Quantile(0.9)));
            json.Append(u",\"0.99\":").Append(LikesProgram::String::Format(u"{:.6f}", Quantile(0.99)));

            if (Count() > 0) {
                const double alpha = m_impl ? m_impl->m_alpha.load(std::memory_order_relaxed) : -1.0;
                if (alpha > 0.0 && alpha < 1.0) {
                    json.Append(u",\"ema\":").Append(LikesProgram::String::Format(u"{:.6f}", EMA()));
                }
                json.Append(u",\"min\":").Append(LikesProgram::String::Format(u"{:.6f}", Min()));
                json.Append(u",\"max\":").Append(LikesProgram::String::Format(u"{:.6f}", Max()));
            }

            json.Append(u"}");
            return json;
        }
    }
}
