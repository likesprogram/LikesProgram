#include <stringFormat/FormatSpec.hpp>

namespace LikesProgram {
    namespace StringFormat {
        struct FormatSpec::FormatSpecImpl {
            // 索引相关
            int m_index = -1;               // 参数索引（若为 -1 表示未显式指定）
            bool m_explicitIndex = false;   // 是否显式指定了 index

            // 填充与对齐
            String m_fill = U" ";           // 填充字符，可多字符（默认空格）
            char32_t m_align = U'>';        // 对齐方式：<' 左, >' 右, ^' 居中, 默认右对齐

            // 符号控制
            char32_t m_sign = 0;            // '+', '-', ' '，0 表示未指定
            bool m_alternateForm = false;        // '#'，显示进制前缀
            bool m_zeroPad = false;         // '0'，零填充

            // 宽度与精度
            std::optional<int> m_width;     // 输出最小宽度（可省略）
            std::optional<int> m_precision; // 精度或字符串截断（可省略）

            // 类型控制
            char32_t m_type = U's';         // 数据类型标识：s, d, x, f, t 等
            String m_typeExpand;            // 类型扩展描述（如时间模板）

            // 内部状态与扩展
            bool m_valid = true;            // 是否解析成功（false 表示格式错误）
            String m_raw;                   // 原始格式子串，便于调试或错误输出

            FormatSpecImpl() = default;
            FormatSpecImpl(int idx, bool exp) : m_index(idx), m_explicitIndex(exp) {}
        };

        // --- 构造 / 析构 ---
        FormatSpec::FormatSpec()
            : m_impl(new FormatSpecImpl()) {
        }

        FormatSpec::FormatSpec(int idx, bool explicitIdx)
            : m_impl(new FormatSpecImpl(idx, explicitIdx)) {
        }

        FormatSpec::~FormatSpec() {
            if (m_impl) {
                delete m_impl;
                m_impl = nullptr;
            }
        }

        // --- 拷贝 / 移动 ---
        FormatSpec::FormatSpec(const FormatSpec& other)
            : m_impl(new FormatSpecImpl(*other.m_impl)) {
        }

        FormatSpec::FormatSpec(FormatSpec&& other) noexcept
            : m_impl(other.m_impl) {
            other.m_impl = nullptr;
        }

        FormatSpec& FormatSpec::operator=(const FormatSpec& other) {
            if (this != &other) {
                if (!m_impl)
                    m_impl = new FormatSpecImpl();
                *m_impl = *other.m_impl;
            }
            return *this;
        }

        FormatSpec& FormatSpec::operator=(FormatSpec&& other) noexcept {
            if (this != &other) {
                if (m_impl)
                    delete m_impl;
                m_impl = other.m_impl;
                other.m_impl = nullptr;
            }
            return *this;
        }

        // --- 重置 ---
        void FormatSpec::Reset() noexcept {
            if (m_impl)
                *m_impl = FormatSpecImpl{};
            else
                m_impl = new FormatSpecImpl();
        }

        // --- Getter / Setter 实现 ---
        int FormatSpec::GetIndex() const noexcept { return m_impl->m_index; }
        void FormatSpec::SetIndex(int idx, bool explicitIdx) {
            m_impl->m_index = idx;
            m_impl->m_explicitIndex = explicitIdx;
        }

        bool FormatSpec::HasExplicitIndex() const noexcept { return m_impl->m_explicitIndex; }

        const String& FormatSpec::GetFill() const noexcept { return m_impl->m_fill; }
        void FormatSpec::SetFill(const String& f) { m_impl->m_fill = f; }

        char32_t FormatSpec::GetAlign() const noexcept { return m_impl->m_align; }
        void FormatSpec::SetAlign(char32_t a) { m_impl->m_align = a; }

        char32_t FormatSpec::GetSign() const noexcept { return m_impl->m_sign; }
        void FormatSpec::SetSign(char32_t s) { m_impl->m_sign = s; }

        bool FormatSpec::GetAlternateForm() const noexcept { return m_impl->m_alternateForm; }
        void FormatSpec::SetAlternateForm(bool show) { m_impl->m_alternateForm = show; }

        bool FormatSpec::GetZeroPad() const noexcept { return m_impl->m_zeroPad; }
        void FormatSpec::SetZeroPad(bool zero) { m_impl->m_zeroPad = zero; }

        std::optional<int> FormatSpec::GetWidth() const noexcept { return m_impl->m_width; }
        void FormatSpec::SetWidth(std::optional<int> w) { m_impl->m_width = std::move(w); }

        std::optional<int> FormatSpec::GetPrecision() const noexcept { return m_impl->m_precision; }
        void FormatSpec::SetPrecision(std::optional<int> p) { m_impl->m_precision = std::move(p); }

        char32_t FormatSpec::GetType() const noexcept { return m_impl->m_type; }
        void FormatSpec::SetType(char32_t t) { m_impl->m_type = t; }

        const String& FormatSpec::GetTypeExpand() const noexcept { return m_impl->m_typeExpand; }
        void FormatSpec::SetTypeExpand(const String& expand) { m_impl->m_typeExpand = expand; }

        bool FormatSpec::IsValid() const noexcept { return m_impl->m_valid; }
        void FormatSpec::SetValid(bool v) { m_impl->m_valid = v; }

        const String& FormatSpec::GetRaw() const noexcept { return m_impl->m_raw; }
        void FormatSpec::SetRaw(const String& raw) { m_impl->m_raw = raw; }
    }
}
