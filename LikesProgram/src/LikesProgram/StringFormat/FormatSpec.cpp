#include "../../../include/LikesProgram/StringFormat/FormatSpec.hpp"

namespace LikesProgram {
	namespace StringFormat {
        struct FormatSpec::FormatSpecImpl {
            // 索引相关
            int index = -1;               // 参数索引（若为 -1 表示未显式指定）
            bool explicitIndex = false;   // 是否显式指定了 index

            // 填充与对齐
            String fill = U" ";           // 填充字符，可多字符（默认空格）
            char32_t align = U'>';        // 对齐方式：<' 左, >' 右, ^' 居中, 默认右对齐

            // 符号控制
            char32_t sign = 0;            // '+', '-', ' '，0 表示未指定
            bool alternateForm = false;        // '#'，显示进制前缀
            bool zeroPad = false;         // '0'，零填充

            // 宽度与精度
            std::optional<int> width;     // 输出最小宽度（可省略）
            std::optional<int> precision; // 精度或字符串截断（可省略）

            // 类型控制
            char32_t type = U's';         // 数据类型标识：s, d, x, f, t 等
            String typeExpand;            // 类型扩展描述（如时间模板）

            // 内部状态与扩展
            bool valid = true;            // 是否解析成功（false 表示格式错误）
            String raw;                   // 原始格式子串，便于调试或错误输出

            FormatSpecImpl() = default;
            FormatSpecImpl(int idx, bool exp) : index(idx), explicitIndex(exp) {}
        };

        // --- 构造 / 析构 ---
        FormatSpec::FormatSpec()
            : impl(new FormatSpecImpl()) {
        }

        FormatSpec::FormatSpec(int idx, bool explicitIdx)
            : impl(new FormatSpecImpl(idx, explicitIdx)) {
        }

        FormatSpec::~FormatSpec() {
            if (impl) {
                delete impl;
                impl = nullptr;
            }
        }

        // --- 拷贝 / 移动 ---
        FormatSpec::FormatSpec(const FormatSpec& other)
            : impl(new FormatSpecImpl(*other.impl)) {
        }

        FormatSpec::FormatSpec(FormatSpec&& other) noexcept
            : impl(other.impl) {
            other.impl = nullptr;
        }

        FormatSpec& FormatSpec::operator=(const FormatSpec& other) {
            if (this != &other) {
                if (!impl)
                    impl = new FormatSpecImpl();
                *impl = *other.impl;
            }
            return *this;
        }

        FormatSpec& FormatSpec::operator=(FormatSpec&& other) noexcept {
            if (this != &other) {
                if (impl)
                    delete impl;
                impl = other.impl;
                other.impl = nullptr;
            }
            return *this;
        }

        // --- 重置 ---
        void FormatSpec::Reset() noexcept {
            if (impl)
                *impl = FormatSpecImpl{};
            else
                impl = new FormatSpecImpl();
        }

        // --- Getter / Setter 实现 ---
        int FormatSpec::GetIndex() const noexcept { return impl->index; }
        void FormatSpec::SetIndex(int idx, bool explicitIdx) {
            impl->index = idx;
            impl->explicitIndex = explicitIdx;
        }

        bool FormatSpec::HasExplicitIndex() const noexcept { return impl->explicitIndex; }

        const String& FormatSpec::GetFill() const noexcept { return impl->fill; }
        void FormatSpec::SetFill(const String& f) { impl->fill = f; }

        char32_t FormatSpec::GetAlign() const noexcept { return impl->align; }
        void FormatSpec::SetAlign(char32_t a) { impl->align = a; }

        char32_t FormatSpec::GetSign() const noexcept { return impl->sign; }
        void FormatSpec::SetSign(char32_t s) { impl->sign = s; }

        bool FormatSpec::GetAlternateForm() const noexcept { return impl->alternateForm; }
        void FormatSpec::SetAlternateForm(bool show) { impl->alternateForm = show; }

        bool FormatSpec::GetZeroPad() const noexcept { return impl->zeroPad; }
        void FormatSpec::SetZeroPad(bool zero) { impl->zeroPad = zero; }

        std::optional<int> FormatSpec::GetWidth() const noexcept { return impl->width; }
        void FormatSpec::SetWidth(std::optional<int> w) { impl->width = std::move(w); }

        std::optional<int> FormatSpec::GetPrecision() const noexcept { return impl->precision; }
        void FormatSpec::SetPrecision(std::optional<int> p) { impl->precision = std::move(p); }

        char32_t FormatSpec::GetType() const noexcept { return impl->type; }
        void FormatSpec::SetType(char32_t t) { impl->type = t; }

        const String& FormatSpec::GetTypeExpand() const noexcept { return impl->typeExpand; }
        void FormatSpec::SetTypeExpand(const String& expand) { impl->typeExpand = expand; }

        bool FormatSpec::IsValid() const noexcept { return impl->valid; }
        void FormatSpec::SetValid(bool v) { impl->valid = v; }

        const String& FormatSpec::GetRaw() const noexcept { return impl->raw; }
        void FormatSpec::SetRaw(const String& raw) { impl->raw = raw; }
	}
}
