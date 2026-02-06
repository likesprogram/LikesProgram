#include "../../../include/LikesProgram/stringFormat/FormatInternal.hpp"
#include <sstream>
#include <iomanip>
#include "../../../include/LikesProgram/time/Time.hpp"

namespace LikesProgram {
    namespace StringFormat {
        FormatInternal::FormatInternal(): m_impl(new FormatInternalImpl()){ }
        FormatInternal::~FormatInternal() {
            if (m_impl) delete m_impl;
            m_impl = nullptr;
        }

        // 拷贝构造
        FormatInternal::FormatInternal(const FormatInternal& other)
            : m_impl(new FormatInternalImpl()) {
            std::shared_lock lock(other.m_impl->m_mutex);
            m_impl->m_nameFormatters = other.m_impl->m_nameFormatters;
            m_impl->m_typeFormatters = other.m_impl->m_typeFormatters;
        }

        // 拷贝赋值
        FormatInternal& FormatInternal::operator=(const FormatInternal& other) {
            if (this == &other) return *this;
            std::unique_lock lockThis(m_impl->m_mutex, std::defer_lock);
            std::shared_lock lockOther(other.m_impl->m_mutex, std::defer_lock);
            std::lock(lockThis, lockOther);
            m_impl->m_nameFormatters = other.m_impl->m_nameFormatters;
            m_impl->m_typeFormatters = other.m_impl->m_typeFormatters;
            return *this;
        }

        // 移动构造
        FormatInternal::FormatInternal(FormatInternal&& other) noexcept
            : m_impl(other.m_impl) {
            other.m_impl = nullptr;
        }

        // 移动赋值
        FormatInternal& FormatInternal::operator=(FormatInternal&& other) noexcept {
            if (this == &other) return *this;
            if (m_impl) delete m_impl;
            m_impl = other.m_impl;
            other.m_impl = nullptr;
            return *this;
        }

        // 单例
        FormatInternal& FormatInternal::Instance() {
            static std::atomic<FormatInternal*> instance{ nullptr };
            static std::mutex mutex;

            // 检查实例是否需要重新创建
            FormatInternal* inst = instance.load(std::memory_order_acquire);
            if (!inst) {
                std::lock_guard lock(mutex);
                inst = instance.load(std::memory_order_relaxed);
                if (!inst) {
                    inst = new FormatInternal(); // 构造函数为私有或受限时，这里也可以访问
                    instance.store(inst, std::memory_order_release);
                }
            }

            return *inst;
        }

        // 注册格式化函数
        void FormatInternal::RegisterFormatter(const std::string& name, UserFormatter func) {
            if (name.empty()) return;
            std::unique_lock lock(m_impl->m_mutex);
            m_impl->m_nameFormatters[name] = std::move(func);
        }
        // 移除按名称的格式化器
        void FormatInternal::UnregisterFormatter(const std::string& name) {
            if (name.empty()) return;
            std::unique_lock lock(m_impl->m_mutex);
            m_impl->m_nameFormatters.erase(name);
        }
        // 查询按名称的格式化器是否存在
        bool FormatInternal::HasFormatter(const std::string& name) const {
            if (name.empty()) return false;
            std::shared_lock lock(m_impl->m_mutex);
            return m_impl->m_nameFormatters.find(name) != m_impl->m_nameFormatters.end();
        }

        // 格式化
        String FormatInternal::FormatAny(const String& fmt, const std::vector<Any>& args) {
            // 解析格式字符串
            FormatParser parser;
            auto res = parser.Parse(fmt);

            if (res.hasFatalError) {
                // 语法错误：直接返回错误标志
                return String(U"{!}");
            }

            // 参数个数
            size_t argCount = args.size();

            // 追踪已使用参数（防止重复分配）
            std::vector<bool> used(argCount, false);
            for (const auto& tok : res.tokens) {
                if (tok.isPlaceholder && tok.spec.HasExplicitIndex()) {
                    int idx = tok.spec.GetIndex();
                    if (idx >= 0 && static_cast<size_t>(idx) < argCount) used[idx] = true;
                }
            }

            size_t nextAuto = 0;
            String output;

            // 逐个处理 token
            for (const auto& tok : res.tokens) {
                if (!tok.isPlaceholder) {
                    // 普通文本直接拼接
                    output = output + tok.literal;
                    continue;
                }

                const FormatSpec& spec = tok.spec;
                int chosenIndex = -1;

                // 显式参数索引
                if (spec.HasExplicitIndex()) {
                    chosenIndex = spec.GetIndex();
                }
                else {
                    // 自动分配下一个未使用参数
                    while (nextAuto < argCount && used[nextAuto]) ++nextAuto;
                    if (nextAuto < argCount) {
                        chosenIndex = static_cast<int>(nextAuto);
                        used[nextAuto] = true;
                        ++nextAuto;
                    }
                    else {
                        // 参数数量不足
                        output = output + String(U"{?}");
                        continue;
                    }
                }

                if (chosenIndex < 0 || static_cast<size_t>(chosenIndex) >= argCount) {
                    output = output + String(U"{?}");
                    continue;
                }

                const Any& anyVal = args[chosenIndex];

                // 格式化单个参数
                String formatted = FormatArgument(&anyVal, spec);

                // 应用对齐和填充规则
                std::wstring utf8 = formatted.ToWString();
                std::wstring finalStd = ApplyAlignmentAndFill(utf8, spec);
                output = output + String(finalStd);
            }

            return output;
        }

        // 参数格式化实现
        String FormatInternal::FormatArgument(const Any* argPtr, const FormatSpec& spec) const {
            if (!argPtr) return String(U"{?}");
            const Any& a = *argPtr;

            // 使用命名格式化器（{:uName}）
            if (spec.GetType() == U'u') {
                std::string name = spec.GetTypeExpand().ToStdString();
                if (!name.empty()) {
                    if (auto out = TryInvokeNamedFormatter(name, a, spec))
                        return *out;
                    return String(U"{!type}");
                }
            }

            // 尝试按类型匹配的注册格式化器
            std::type_index tid = std::type_index(a.type());
            if (auto out = TryInvokeTypeFormatter(tid, a, spec)) return *out;

            // 内置类型的通用格式化（int、double、bool 等）
            if (auto bs = FormatBuiltInToStdString(a, spec)) {
                return String(*bs);
            }

            // 尝试匹配String构造器
            String str;
            if (String::FromAny(a, str)) return str;

            // 尝试 operator<< 输出（用于未注册的可流式输出类型）
            try {
                std::wostringstream oss;
                oss << L"0x" << std::hex << reinterpret_cast<uintptr_t>(std::any_cast<void*>(a));
                return String(oss.str());
            }
            catch (...) {}

            // 5. 全部失败则返回占位符
            return String(U"{!type}");
        }

        // 内建类型格式化（返回 std::nullopt 表示不处理）
        std::optional<std::wstring> FormatInternal::FormatBuiltInToStdString(const Any& a, const FormatSpec& spec) const
        {
            try {
                char32_t type = spec.GetType();

                // ---------- 整型 ----------
                if (a.type() == typeid(int) || a.type() == typeid(short) ||
                    a.type() == typeid(long) || a.type() == typeid(long long))
                {
                    long long v = 0;
                    if (a.type() == typeid(int)) v = std::any_cast<int>(a);
                    else if (a.type() == typeid(short)) v = std::any_cast<short>(a);
                    else if (a.type() == typeid(long)) v = std::any_cast<long>(a);
                    else v = std::any_cast<long long>(a);

                    std::wostringstream oss;
                    bool upperHex = (type == U'X');

                    // 十六进制
                    if (type == U'x' || type == U'X') {
                        if (spec.GetAlternateForm()) oss << (upperHex ? L"0X" : L"0x");
                        oss << std::hex << (upperHex ? std::uppercase : std::nouppercase)
                            << static_cast<unsigned long long>(v);
                    }
                    // 二进制
                    if (type == U'b' || type == U'B') {
                        unsigned long long u = static_cast<unsigned long long>(v);
                        std::wstring bits;
                        while (u) { bits.push_back(L'0' + (u & 1)); u >>= 1; }
                        if (bits.empty()) bits = L"0";
                        std::reverse(bits.begin(), bits.end());
                        if (spec.GetAlternateForm()) bits = (type == U'B' ? L"0B" : L"0b") + bits;
                        return bits;
                    }
                    // 八进制
                    if (type == U'o' || type == U'O') {
                        std::wostringstream tmp;
                        tmp << std::oct << v;
                        std::wstring s = tmp.str();
                        if (spec.GetAlternateForm()) s = (type == U'O' ? L"0O" : L"0o") + s;
                        return s;
                    }
                    // 十进制
                    if (type == U'd' || type == U'i') {
                        oss << v;
                        return oss.str();
                    }
                    else {
                        oss << v;
                    }
                    return oss.str();
                }

                // ---------- 无符号整型 ----------
                if (a.type() == typeid(unsigned int) ||
                    a.type() == typeid(unsigned long) ||
                    a.type() == typeid(unsigned long long))
                {
                    unsigned long long v = 0;
                    if (a.type() == typeid(unsigned int)) v = std::any_cast<unsigned int>(a);
                    else if (a.type() == typeid(unsigned long)) v = std::any_cast<unsigned long>(a);
                    else v = std::any_cast<unsigned long long>(a);

                    std::wostringstream oss;

                    // 十六进制
                    if (type == U'x' || type == U'X') {
                        if (spec.GetAlternateForm()) oss << (type == U'X' ? L"0X" : L"0x");
                        oss << std::hex << (type == U'X' ? std::uppercase : std::nouppercase) << v;
                    }
                    // 二进制
                    if (type == U'b' || type == U'B') {
                        unsigned long long u = v;
                        std::wstring bits;
                        while (u) { bits.push_back(L'0' + (u & 1)); u >>= 1; }
                        if (bits.empty()) bits = L"0";
                        std::reverse(bits.begin(), bits.end());
                        if (spec.GetAlternateForm()) bits = (type == U'B' ? L"0B" : L"0b") + bits;
                        return bits;
                    }
                    // 八进制
                    if (type == U'o' || type == U'O') {
                        std::wostringstream tmp;
                        tmp << std::oct << v;
                        std::wstring s = tmp.str();
                        if (spec.GetAlternateForm()) s = (type == U'O' ? L"0O" : L"0o") + s;
                        return s;
                    }
                    // 十进制
                    if (type == U'd' || type == U'i') {
                        oss << v;
                        return oss.str();
                    }
                    else {
                        oss << v;
                    }
                    return oss.str();
                }

                // ---------- 浮点 ----------
                if (a.type() == typeid(float) || a.type() == typeid(double) || a.type() == typeid(long double))
                {
                    long double v = 0.0;
                    if (a.type() == typeid(float)) v = std::any_cast<float>(a);
                    else if (a.type() == typeid(double)) v = std::any_cast<double>(a);
                    else v = std::any_cast<long double>(a);

                    std::wostringstream oss;
                    int prec = spec.GetPrecision().value_or(6);

                    if (type == U'e' || type == U'E')
                        oss << std::scientific << std::setprecision(prec) << static_cast<double>(v);
                    else if (type == U'g' || type == U'G')
                        oss << std::defaultfloat << std::setprecision(prec) << static_cast<double>(v);
                    else if (type == U'%')
                        oss << std::fixed << std::setprecision(prec) << static_cast<double>(v) * 100.0 << L"%";
                    else // f/F
                        oss << std::fixed << std::setprecision(prec) << static_cast<double>(v);

                    return oss.str();
                }

                // ---------- 布尔 ----------
                if (a.type() == typeid(bool))
                    return std::any_cast<bool>(a) ? L"true" : L"false";

                // ---------- 字符 ----------
                if (a.type() == typeid(char) || a.type() == typeid(wchar_t)) {
                    wchar_t ch = (a.type() == typeid(char)) ? static_cast<wchar_t>(std::any_cast<char>(a))
                        : std::any_cast<wchar_t>(a);
                    return std::wstring(1, ch);
                }
                if (type == U'c') {
                    if (a.type() == typeid(int)) return std::wstring(1, static_cast<wchar_t>(std::any_cast<int>(a)));
                }

                // ---------- 字符串 ----------
                if (a.type() == typeid(String)) return std::any_cast<String>(a).ToWString();
                if (a.type() == typeid(std::wstring)) return std::any_cast<std::wstring>(a);
                if (a.type() == typeid(const wchar_t*)) return std::wstring(std::any_cast<const wchar_t*>(a));
                if (a.type() == typeid(wchar_t*)) return std::wstring(std::any_cast<wchar_t*>(a));
                if (a.type() == typeid(std::string))
                    return String(std::any_cast<std::string>(a), String::Encoding::UTF8).ToWString();
                if (a.type() == typeid(const char*))
                    return String(std::any_cast<const char*>(a), String::Encoding::UTF8).ToWString();
                if (a.type() == typeid(char*))
                    return String(std::any_cast<char*>(a), String::Encoding::UTF8).ToWString();

                // ---------- 时间 ----------
                if (a.type() == typeid(LikesProgram::Time::TimePoint))
                    if (spec.GetTypeExpand().Empty()) return LikesProgram::Time::FormatTime(std::any_cast<LikesProgram::Time::TimePoint>(a)).ToWString();
                    else return LikesProgram::Time::FormatTime(std::any_cast<LikesProgram::Time::TimePoint>(a), spec.GetTypeExpand()).ToWString();

                // ---------- 指针 ----------
                if (type == U'p' || type == U'P') {
                    if (a.type() == typeid(void*)) {
                        std::wostringstream oss;
                        oss << a.type().hash_code(); // 或者使用 uintptr_t 显示地址
                        return oss.str();
                    }
                }
            }
            catch (...) {}
            return std::nullopt;
        }

        // // 调用 name 注册表
        std::optional<String> FormatInternal::TryInvokeNamedFormatter(const std::string& name, const Any& a, const FormatSpec& spec) const {
            std::shared_lock lock(m_impl->m_mutex);
            auto it = m_impl->m_nameFormatters.find(name);
            if (it == m_impl->m_nameFormatters.end()) return std::nullopt;
            try {
                return it->second(a, spec);
            }
            catch (...) {
                return std::nullopt;
            }
        }

        // 调用 typeid 注册表
        std::optional<String> FormatInternal::TryInvokeTypeFormatter(const std::type_index& ti, const Any& a, const FormatSpec& spec) const {
            std::shared_lock lock(m_impl->m_mutex);
            auto it = m_impl->m_typeFormatters.find(ti);
            if (it == m_impl->m_typeFormatters.end()) return std::nullopt;
            try {
                return it->second(a, spec);
            }
            catch (...) {
                return std::nullopt;
            }
        }

        // 对结果应用对齐/填充/宽度（result is UTF-8 std::wstring）
        std::wstring FormatInternal::ApplyAlignmentAndFill(const std::wstring& src, const FormatSpec& spec) const {
            int width = spec.GetWidth().value_or(0);
            if (width <= 0) return src;

            std::wstring fill = spec.GetFill().ToWString();
            if (fill.empty()) fill = L" ";

            size_t fillLen = fill.size();
            size_t srcLen = src.size();
            if ((int)srcLen >= width) return src;

            int padUnits = width - static_cast<int>(srcLen);
            size_t padCount = (fillLen == 0) ? padUnits : (padUnits + fillLen - 1) / fillLen;

            // 如果是零填充模式（如 "%05d"）
            if (spec.GetZeroPad() && spec.GetAlign() == U'>') {
                std::wstring zeros(padUnits, '0');
                return zeros + src;
            }

            std::wstring padding = RepeatFillToLen(fill, padCount);

            switch (spec.GetAlign()) {
            case U'<': return src + padding;
            case U'^': {
                int leftPad = padUnits / 2;
                int rightPad = padUnits - leftPad;
                size_t leftCount = (fillLen == 0) ? leftPad : (leftPad + fillLen - 1) / fillLen;
                size_t rightCount = (fillLen == 0) ? rightPad : (rightPad + fillLen - 1) / fillLen;
                return RepeatFillToLen(fill, leftCount) + src + RepeatFillToLen(fill, rightCount);
            }
            case U'>':
            default:
                return padding + src;
            }
        }

        std::wstring FormatInternal::RepeatFillToLen(const std::wstring& fill, size_t count) {
            if (fill.empty()) return std::wstring(count, ' ');
            std::wstring out;
            out.reserve(fill.size() * count);
            for (size_t i = 0; i < count; ++i) out += fill;
            return out;
        }
    }
}