#include <LikesProgram/Metrics/Metrics.hpp>
#include <LikesProgram/Core/Version.hpp>

#include <utility>

namespace LikesProgram {
    namespace Metrics {
        struct Metric::MetricImpl {
            std::map<LikesProgram::String, LikesProgram::String> m_labels; // 指标标签，按 key 稳定排序
        };

        const char* PackageName() noexcept {
            // 包名固定为 C 字符串，避免跨动态库传递分配所有权。
            return "LikesProgramMetrics";
        }

        const char* PackageVersion() noexcept {
            // Metrics 跟随 LikesProgram 统一版本，发布批次整体校验。
            return LikesProgram::Version::CurrentString().data();
        }

        bool PackageAvailable() noexcept {
            // 导出稳定符号，供测试和诊断工具确认 target 已链接。
            return true;
        }

        Metric::Metric(const LikesProgram::String& name, const LikesProgram::String& help,
            const std::map<LikesProgram::String, LikesProgram::String>& labels)
            : m_name(name), m_help(help), m_impl(new MetricImpl{}) {
            // 构造阶段统一复制标签，保证后续导出顺序由 std::map 固定。
            m_impl->m_labels = labels;
        }

        Metric::~Metric() {
            // PImpl 只在本包内分配和释放，减少 ABI 边界所有权问题。
            delete m_impl;
            m_impl = nullptr;
        }

        Metric::Metric(const Metric& other)
            : m_name(other.m_name), m_help(other.m_help), m_impl(new MetricImpl{}) {
            // 源对象可能是 moved-from，空实现时保留空标签。
            if (other.m_impl) m_impl->m_labels = other.m_impl->m_labels;
        }

        Metric& Metric::operator=(const Metric& other) {
            if (this == &other) return *this;

            m_name = other.m_name;
            m_help = other.m_help;
            MutableLabels() = other.m_impl
                ? other.m_impl->m_labels
                : std::map<LikesProgram::String, LikesProgram::String>{};
            return *this;
        }

        Metric::Metric(Metric&& other) noexcept
            : m_name(std::move(other.m_name)),
            m_help(std::move(other.m_help)),
            m_impl(other.m_impl) {
            // 源对象置空后仍可安全析构，必要时 MutableLabels 会重建实现。
            other.m_impl = nullptr;
        }

        Metric& Metric::operator=(Metric&& other) noexcept {
            if (this == &other) return *this;

            delete m_impl;
            m_name = std::move(other.m_name);
            m_help = std::move(other.m_help);
            m_impl = other.m_impl;
            other.m_impl = nullptr;
            return *this;
        }

        LikesProgram::String Metric::FormatLabels(
            const std::map<LikesProgram::String, LikesProgram::String>& labels) {
            LikesProgram::String result; // Prometheus 标签块，空标签保持空串
            bool first = true;           // 控制逗号分隔

            for (const auto& [key, value] : labels) {
                // 空 key 不导出，避免生成非法 Prometheus 标签。
                if (key.Empty()) continue;
                if (first) {
                    result.Append(u"{");
                }
                else {
                    result.Append(u",");
                }

                result.Append(key).Append(u"=\"")
                    .Append(LikesProgram::String::EscapeJson(value)).Append(u"\"");
                first = false;
            }

            if (!first) result.Append(u"}");
            return result;
        }

        std::map<LikesProgram::String, LikesProgram::String>& Metric::MutableLabels() {
            // moved-from 对象被再次赋值时，延迟恢复标签容器。
            if (!m_impl) m_impl = new MetricImpl{};
            return m_impl->m_labels;
        }

        std::map<LikesProgram::String, LikesProgram::String> Metric::LabelsCopy() const {
            // 返回副本，避免调用方拿到内部容器并造成并发风险。
            if (!m_impl) return {};
            return m_impl->m_labels;
        }

        void Metric::SetLabels(const std::map<LikesProgram::String, LikesProgram::String>& labels) {
            // 统一替换标签，用于构造和复制恢复。
            MutableLabels() = labels;
        }
    }
}
