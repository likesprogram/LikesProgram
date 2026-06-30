#pragma once
#include <LikesProgram/Core/system/LikesProgramCoreExport.hpp>
#include <array>
#include <vector>
#include <string>
#include <string_view>
#include <memory>
#include <sstream>
#include <any>
#include <type_traits>
#include <tuple>
#include <typeindex>
#include <utility>

namespace LikesProgram {
    using Any = std::any;

    namespace StringFormat {
        // 零拷贝格式化参数视图，延迟到需要回退时再 materialize 为 Any。
        struct FormatArgView {
            const void* value = nullptr;             // 参数对象地址，不拥有生命周期
            std::type_index type{ typeid(void) };    // 参数的运行时类型标识
            Any(*makeAny)(const void*) = nullptr;    // 兼容旧 Any 路径的惰性构造函数
        };

        // 非指针参数按值复制到 Any，保证注册表回调拿到稳定对象。
        template <typename T>
        Any MakeFormatArgAny(const void* value) {
            return Any(*static_cast<const T*>(value));
        }

        // 指针参数保留原始指针值，避免把指向内容误复制为对象。
        template <typename P>
        Any MakePointerFormatArgAny(const void* value) {
            if constexpr (std::is_const_v<std::remove_pointer_t<P>>) {
                return Any(reinterpret_cast<P>(value));
            }
            else {
                return Any(reinterpret_cast<P>(const_cast<void*>(value)));
            }
        }

        // 为 Format 快路径构造参数视图，数组/指针和对象引用分开处理。
        template <typename T>
        FormatArgView MakeFormatArgView(T&& value) {
            using Raw = std::remove_reference_t<T>;  // 保留数组信息用于识别字面量
            using Decayed = std::decay_t<T>;         // 传入格式化器的稳定类型标识
            if constexpr (std::is_array_v<Raw> || std::is_pointer_v<Decayed>) {
                return FormatArgView{
                    static_cast<const void*>(value),
                    std::type_index(typeid(Decayed)),
                    &MakePointerFormatArgAny<Decayed>
                };
            }
            else {
                return FormatArgView{
                    static_cast<const void*>(std::addressof(value)),
                    std::type_index(typeid(Decayed)),
                    &MakeFormatArgAny<Decayed>
                };
            }
        }
    }

    // UTF-16 存储的 Unicode 字符串，公共索引按 code point 语义暴露。
    class LIKESPROGRAM_CORE_API String {
    public:
        // 支持的编码类型，仅用于标识字符串来源，内部存储总是 UTF-16。
        enum class Encoding { GBK, UTF8, UTF16, UTF32 };
        static constexpr size_t npos = static_cast<size_t>(-1); // 查找失败或默认末尾位置

        // 尝试从 std::any 中提取常见文本类型，失败时不修改输出参数。
        static bool FromAny(const std::any& a, String& outContent);

        // 构造空字符串，并初始化为可直接 data()/c_str() 的 UTF-16 NUL 缓冲。
        String();
        // 按指定窄字符编码解码 NUL 结尾字符串，默认按 UTF-8 处理。
        String(const char* s, Encoding enc = Encoding::UTF8);
        // 按 UTF-8 解码 char8_t NUL 结尾字符串，非法序列由转换层拒绝。
        String(const char8_t* s);
        // 拷贝 UTF-16 NUL 结尾字符串，内部仍追加独立终止符。
        String(const char16_t* s);
        // 拷贝指定长度的 UTF-16 缓冲，可包含中间 NUL 字符。
        String(const char16_t* s, size_t length);
        // 将 UTF-32 NUL 结尾字符串转写为 UTF-16，并校验 Unicode 范围。
        String(const char32_t* s);
        // 深拷贝另一个 String 的内容、编码标识和可用缓存状态。
        String(const String& other);
        // 移动接管另一个 String 的实现对象，并把源对象重置为空串。
        String(String&& other) noexcept;
        // 按指定窄字符编码构造单字符字符串，默认按 UTF-8 单字节输入。
        String(const char c, Encoding enc = Encoding::UTF8);
        // 构造单个 UTF-8 code unit 字符串，适用于 ASCII/单字节字面量。
        String(const char8_t c);
        // 构造单个 UTF-16 code unit 字符串，不组合 surrogate pair。
        String(const char16_t c);
        // 重复写入同一个 UTF-16 code unit，主要用于填充和缩进。
        String(const size_t count, const char16_t c);
        // 构造单个 Unicode code point，补充平面字符会写入 surrogate pair。
        String(const char32_t c);
        // 重复写入同一个 Unicode code point，并按 UTF-16 长度预分配。
        String(const size_t count, const char32_t c);

        // 按指定窄字符编码解码 std::string，默认处理 UTF-8 字节串。
        String(const std::string& s, Encoding enc = Encoding::UTF8);
        // 按指定窄字符编码解码 string_view，不要求输入 NUL 结尾。
        String(std::string_view s, Encoding enc = Encoding::UTF8);
        // 从 std::u8string 拷贝 UTF-8 文本并转为内部 UTF-16。
        String(const std::u8string& s);
        // 从 UTF-8 view 构造，避免临时 std::u8string 分配。
        String(std::u8string_view s);
        // 从 std::wstring 构造，按平台 wchar_t 宽度选择 UTF-16/UTF-32 路径。
        String(const std::wstring& s);
        // 从 wstring_view 构造，避免临时宽字符串分配。
        String(std::wstring_view s);
        // 从 std::u16string 拷贝 UTF-16 内容并保证内部终止符。
        String(const std::u16string& s);
        // 从 UTF-16 view 构造，允许输入包含中间 NUL。
        String(std::u16string_view s);
        // 从 std::u32string 构造，并严格校验 surrogate 与最大码点边界。
        String(const std::u32string& s);
        // 从 UTF-32 view 构造，避免临时 std::u32string 分配。
        String(std::u32string_view s);

        // 将有符号整数格式化为十进制文本。
        String(int64_t value);
        // 将无符号整数格式化为十进制文本。
        String(uint64_t value);
        // 将浮点数格式化为文本，精度由实现层统一控制。
        String(long double value);
        // bool 构造输出 true/false 文本，便于格式化互操作。
        String(bool value);
        // 释放 PImpl 拥有的 UTF-16 缓冲与缓存结构。
        ~String();

        // 从另一个 String 拷贝赋值。
        String& operator=(const String& other);
        // 从另一个 String 移动赋值。
        String& operator=(String&& other) noexcept;
        // 从 UTF-8/指定编码 C 字符串赋值。
        String& operator=(const char* s);
        // 从 UTF-8 char8_t C 字符串赋值。
        String& operator=(const char8_t* s);
        // 从 UTF-16 C 字符串赋值。
        String& operator=(const char16_t* s);
        // 从 UTF-32 C 字符串赋值。
        String& operator=(const char32_t* s);
        // 从 std::string 按 UTF-8 赋值。
        String& operator=(const std::string& s);
        // 从 std::string_view 按 UTF-8 赋值。
        String& operator=(std::string_view s);
        // 从 std::u8string 赋值。
        String& operator=(const std::u8string& s);
        // 从 std::u8string_view 赋值。
        String& operator=(std::u8string_view s);
        // 从 std::wstring 按平台 wchar_t 宽度赋值。
        String& operator=(const std::wstring& s);
        // 从 std::wstring_view 按平台 wchar_t 宽度赋值。
        String& operator=(std::wstring_view s);
        // 从 std::u16string 赋值。
        String& operator=(const std::u16string& s);
        // 从 std::u16string_view 赋值。
        String& operator=(std::u16string_view s);
        // 从 std::u32string 赋值。
        String& operator=(const std::u32string& s);
        // 从 std::u32string_view 赋值。
        String& operator=(std::u32string_view s);

        // Unicode code point 数，首次计算后缓存到下次修改。
        size_t Size() const;
        // UTF-16 code unit 数，与内部存储长度一致。
        size_t Length() const;
        // std 兼容入口：返回 UTF-16 code unit 数，不等同于 Unicode code point 数。
        size_t size() const { return Length(); }
        // std 兼容入口：与 size() 一致，保留 basic_string 迁移语义。
        size_t length() const { return Length(); }
        // 判断是否没有任何 UTF-16 code unit。
        bool Empty() const;
        // std 兼容入口：与 Empty() 等价，供泛型代码调用。
        bool empty() const { return Empty(); }
        // 清空字符串并保留可用 NUL 终止缓冲，同时失效 code point 缓存。
        void Clear();
        // std 兼容入口：与 Clear() 等价。
        void clear() { Clear(); }
        // 返回内部 UTF-16 缓冲区，只读且以 NUL 结尾。
        const char16_t* data() const noexcept;
        // 与 data() 等价，便于 std 风格调用点使用。
        const char16_t* c_str() const noexcept;

        // 按 code point 索引读取字符，越界时由 At() 的规则处理。
        char32_t operator[](size_t index) const;
        // 按 code point 索引安全访问字符，越界抛出 out_of_range。
        char32_t At(size_t index) const;
        // 返回第一个 Unicode code point，空串时抛出 out_of_range。
        char32_t Front() const;
        // 返回最后一个 Unicode code point，空串时抛出 out_of_range。
        char32_t Back() const;

        // 追加另一个 String 的 UTF-16 内容，并使 code point 缓存失效。
        String& Append(const String& str);
        // 拼接单个 UTF-16 code unit。
        String& Append(char16_t c);
        // 拼接单个 Unicode code point，必要时写入 surrogate pair。
        String& Append(char32_t c);
        // 直接拼接 UTF-16 视图，避免临时 String。
        String& Append(std::u16string_view str);
        // 直接拼接 wchar_t 视图，按平台 wchar_t 宽度转换。
        String& Append(std::wstring_view str);
        // std 风格小写别名，方便迁移 basic_string 调用习惯。
        // std 兼容入口：追加 String 并返回自身。
        String& append(const String& str) { return Append(str); }
        // std 兼容入口：追加单个 UTF-16 code unit。
        String& append(char16_t c) { return Append(c); }
        // std 兼容入口：追加单个 Unicode code point。
        String& append(char32_t c) { return Append(c); }
        // std 兼容入口：追加 UTF-16 view，不创建临时 String。
        String& append(std::u16string_view str) { return Append(str); }
        // std 兼容入口：追加 wchar_t view，并按平台宽度转换。
        String& append(std::wstring_view str) { return Append(str); }
        // 追加另一个 String 并返回自身。
        String& operator+=(const String& str);
        // 返回两个 String 拼接后的新对象。
        friend LIKESPROGRAM_CORE_API String operator+(const String& lhs, const String& rhs);

        // 输入输出重载
        friend LIKESPROGRAM_CORE_API std::ostream& operator<<(std::ostream& os, const String& str);
        // 从窄字符输入流读取一行并按当前编码解析。
        friend LIKESPROGRAM_CORE_API std::istream& operator>>(std::istream& is, String& str);
        // 将字符串写入宽字符输出流。
        friend LIKESPROGRAM_CORE_API std::wostream& operator<<(std::wostream& os, const String& str);
        // 从宽字符输入流读取一行。
        friend LIKESPROGRAM_CORE_API std::wistream& operator>>(std::wistream& is, String& str);

        // 按 code point 区间截取子串，内部会转换为 UTF-16 边界。
        String SubString(size_t index, size_t count) const;
        // 返回左侧 count 个 code point，count 超界时返回自身副本。
        String Left(size_t count) const;
        // 返回右侧 count 个 code point，count 超界时返回自身副本。
        String Right(size_t count) const;

        // 返回 Unicode 大写映射后的新字符串，原对象不变。
        String ToUpper() const;
        // 返回 Unicode 小写映射后的新字符串，原对象不变。
        String ToLower() const;
        // 原地执行 Unicode 大写映射，并使缓存失效。
        void ToUpperInPlace();
        // 原地执行 Unicode 小写映射，并使缓存失效。
        void ToLowerInPlace();

        // 从指定 code point 位置起查找子串，未找到返回 npos。
        size_t Find(const String& str, size_t start = 0) const;
        // 从指定 code point 位置向前查找子串，默认从末尾开始。
        size_t LastFind(const String& str, size_t start = npos) const;
        // 判断当前字符串是否以指定子串开头。
        bool StartsWith(const String& str) const;
        // 判断当前字符串是否以指定子串结尾。
        bool EndsWith(const String& str) const;

        // 通过 Unicode 大小写归一化比较两个字符串是否相等。
        bool EqualsIgnoreCase(const String& other) const;
        // 比较运算按 code point 顺序执行。
        bool operator==(const String& other) const;
        // 判断两个字符串是否不相等。
        bool operator!=(const String& other) const;
        // 按 code point 字典序判断小于。
        bool operator<(const String& other) const;
        // 按 code point 字典序判断小于等于。
        bool operator<=(const String& other) const;
        // 按 code point 字典序判断大于。
        bool operator>(const String& other) const;
        // 按 code point 字典序判断大于等于。
        bool operator>=(const String& other) const;

        // 按指定编码导出窄字符字节串，默认导出 UTF-8。
        std::string ToStdString(Encoding enc = Encoding::UTF8) const;
        // 转换为 C++20 UTF-8 字符串。
        std::u8string ToU8String() const;
        // 导出 std::wstring，按平台 wchar_t 宽度选择 UTF-16 或 UTF-32。
        std::wstring ToWString() const;
        // 导出内部 UTF-16 内容副本，不包含额外 NUL 终止符。
        std::u16string ToU16String() const;
        // 导出 UTF-32 code point 序列，surrogate pair 会被合并。
        std::u32string ToU32String() const;

        // 隐式转换保留 std 兼容性，内部仍以 UTF-16 作为唯一存储。
        operator std::string() const { return ToStdString(); }
        operator std::u8string() const { return ToU8String(); }
        operator std::wstring() const { return ToWString(); }
        // UTF-16/UTF-32 隐式转换服务于 std 容器与测试断言。
        operator std::u16string() const { return ToU16String(); }
        operator std::u32string() const { return ToU32String(); }

        // 按指定分隔符拆分字符串，空分隔符时返回原串。
        std::vector<String> Split(const String& sep) const;

        // 只读 code point 迭代器，用于 range-for 访问 Unicode 字符。
        class CodePointIterator {
        private:
            const String* str; // 被遍历字符串，不拥有生命周期
            size_t idx;        // 当前 code point 索引
        public:
            // 创建指向指定 code point 索引的轻量迭代器。
            CodePointIterator(const String* s, size_t i) : str(s), idx(i) {}
            // 解引用返回当前 Unicode code point。
            char32_t operator*() const { return str->At(idx); }
            // 前进到下一个 code point。
            CodePointIterator& operator++() { ++idx; return *this; }
            // 只比较索引，调用点负责保证迭代器来自同一字符串。
            bool operator!=(const CodePointIterator& other) const { return idx != other.idx; }
        };

        // std/range-for 兼容入口：返回 code point 序列起点。
        CodePointIterator begin() const { return CodePointIterator(this, 0); }
        // std/range-for 兼容入口：返回 code point 序列尾后位置。
        CodePointIterator end() const { return CodePointIterator(this, Size()); }

        // 生成 JSON 字符串内容转义后的文本，不包含外层引号。
        static String EscapeJson(const String& str);

        // 使用 String 格式串和类型擦除视图进行格式化，避免热路径 std::any 分配。
        template <typename... Args>
        static String Format(const String& fmt, Args&&... args) {
            std::array<StringFormat::FormatArgView, sizeof...(Args)> views{ { // 参数视图数组，避免 std::any 热路径分配
                StringFormat::MakeFormatArgView(args)...
            } };
            return FormatViews(fmt, views.data(), views.size());
        }

        // 使用 UTF-16 view 作为格式串，内部缓存为 String 后复用主格式化入口。
        template <typename... Args>
        static String Format(std::u16string_view fmt, Args&&... args) {
            return Format(CachedFormatString(fmt), std::forward<Args>(args)...);
        }

        // 使用 UTF-32 view 作为格式串，适合直接传入 U"" 字面量视图。
        template <typename... Args>
        static String Format(std::u32string_view fmt, Args&&... args) {
            return Format(CachedFormatString(fmt), std::forward<Args>(args)...);
        }

        // 使用 UTF-16 字面量格式串，自动剔除末尾 NUL。
        template <size_t N, typename... Args>
        static String Format(const char16_t(&fmt)[N], Args&&... args) {
            return Format(std::u16string_view(fmt, N > 0 ? N - 1 : 0), std::forward<Args>(args)...);
        }

        // 使用 UTF-32 字面量格式串，自动剔除末尾 NUL。
        template <size_t N, typename... Args>
        static String Format(const char32_t(&fmt)[N], Args&&... args) {
            return Format(std::u32string_view(fmt, N > 0 ? N - 1 : 0), std::forward<Args>(args)...);
        }

        // 将常见标量、可转换文本或可流输出类型转换为 String，失败返回空串。
        template <typename T>
        static String ToString(const T& value) {
            try {
                if constexpr (std::is_convertible_v<T, String>) {
                    // 如果类型可直接转换为 String，直接返回
                    return String(value);
                }
                else if constexpr (
                    std::is_integral_v<T> ||
                    std::is_floating_point_v<T> ||
                    std::is_enum_v<T> ||
                    std::is_same_v<T, bool> ||
                    std::is_pointer_v<T>) {
                    return String::Format(u"{}", value);
                } else {
                    // 其他类型，尝试转换为字符串
                    std::wstringstream ss; // 用户类型的宽字符流回退输出
                    ss << value;
                    return String(ss.str());
                }
            }
            catch (const std::exception&) {
                // 如果所有转换方法都失败，返回空字符串
                return String();
            }
        }

    private:
        // PImpl 隐藏存储、缓存和同步细节，保持 ABI 稳定。
        struct StringImpl;
        StringImpl* m_impl = nullptr; // 唯一拥有的实现对象，析构时释放

        // 将 code point 索引转换为 UTF-16 code unit 偏移。
        size_t CodePointOffset(size_t index) const;

        // 修改内容后使 code point 缓存失效。
        void InvalidateCodePointCache();

        // 只刷新 code point 数量缓存。
        void UpdateCodePointCount() const;
        // 刷新完整 code point 偏移表缓存。
        void UpdateCodePointCache() const;

        // 兼容旧 std::any 调用路径的格式化入口。
        static String FormatAny(const String& fmt, const std::vector<Any>& args);
        // 快路径格式化入口，避免每个参数都构造 std::any。
        static String FormatViews(const String& fmt, const StringFormat::FormatArgView* args, size_t argCount);
        // 缓存 UTF-16 字面量格式串，避免重复构造 String。
        static const String& CachedFormatString(std::u16string_view fmt);
        // 缓存 UTF-32 字面量格式串，避免重复构造 String。
        static const String& CachedFormatString(std::u32string_view fmt);
    };
}

namespace std {
    template<>
    // 允许 String 作为 unordered 容器键，哈希按 code point 计算。
    struct hash<LikesProgram::String> {
        size_t operator()(const LikesProgram::String& s) const noexcept {
            size_t h = 0; // 滚动哈希累积值
            for (auto cp : s) h = h * 31 + static_cast<size_t>(cp);
            return h;
        }
    };
}
