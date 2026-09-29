#include "LikesProgram/Config/ConfigurationInternal.hpp"
#include <array>
#include <charconv>
#include <limits>
#include <optional>
#include <type_traits>
#include <variant>

#if defined(_MSC_VER)
#define LP_CONFIG_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define LP_CONFIG_NOINLINE __attribute__((noinline))
#else
#define LP_CONFIG_NOINLINE
#endif

namespace LikesProgram {
    namespace Config {
        namespace Internal {
            namespace {
                // 递归下降 JSON5 解析器；只依赖 Core/String，不引入第三方 JSON 库。
                class JsonParser {
                public:
                    // JSON 语法标记均为 ASCII，直接使用 Core 的 UTF-16 存储避免整份文档转成 UTF-32。
                    explicit JsonParser(const String& text) : m_text(text.data(), text.Length()) { }

                    // 解析完整 JSON5 文档，额外内容会作为错误返回。
                    Result<ConfigValue> Parse() {
                        try {
                            SkipWhitespace();
                            if (m_pos < m_text.size() && IsJson5Quote(m_text[m_pos])) {
                                auto text = m_text[m_pos] == u'"' ? ParseString<u'"'>() : ParseString<u'\''>(); // 根字符串专用快路
                                if (!text) return Error(m_errorMessage, m_pos);

                                ConfigValue value = MakeParsedRootString(std::move(*text)); // 唯一公开根
                                SkipWhitespace();
                                if (m_pos != m_text.size()) return TrailingDataError();
                                return value;
                            }

                            if (m_pos < m_text.size() && m_text[m_pos] == U'[') {
                                const size_t begin = m_pos; // 非空数组回退到统一递归解析的位置
                                ++m_pos;
                                SkipWhitespace();
                                if (m_pos < m_text.size() && m_text[m_pos] == U']') {
                                    ++m_pos;
                                    SkipWhitespace();
                                    if (m_pos != m_text.size()) return TrailingDataError();
                                    return ConfigValue::Array();
                                }
                                m_pos = begin;
                            }

                            auto value = ParseValueAfterWhitespace(); // 根节点已完成前导空白扫描
                            if (!value) return Error(m_errorMessage, m_pos);
                            SkipWhitespace();
                            if (m_pos != m_text.size()) return TrailingDataError();
                            return ConfigValueAccess::FromNode(std::move(*value));
                        } catch (const std::exception& ex) {
                            // 内存或 Core 转换等未预期异常仍保留诊断，不伪装成普通语法失败。
                            return Error(String(ex.what()), m_pos);
                        }
                    }

                private:
                    using ParsedString = std::variant<std::u16string_view, std::u16string>;

                    static ConfigNode MakeParsedStringValue(ParsedString&& text) {
                        if (const auto* view = std::get_if<std::u16string_view>(&text)) return ConfigNode(ConfigText(*view));
                        return ConfigNode(ConfigText(std::move(std::get<std::u16string>(text))));
                    }

                    // 根字符串只分配公开 PImpl 一次，不经过 optional<ConfigNode> 搬移。
                    static ConfigValue MakeParsedRootString(ParsedString&& text) {
                        if (const auto* view = std::get_if<std::u16string_view>(&text)) return ConfigValueAccess::FromText(*view);
                        return ConfigValueAccess::FromText(std::move(std::get<std::u16string>(text)));
                    }

                    static void SetParsedEntry(
                        ConfigObject& object, ParsedString&& key, ConfigNode&& value) {
                        const auto keyView = std::holds_alternative<std::u16string_view>(key) ? std::get<std::u16string_view>(key) : std::u16string_view(std::get<std::u16string>(key)); // 覆盖前查询 view
                        for (auto& entry : object) {
                            if (entry.key.View() == keyView) {
                                entry.value = std::move(value);
                                return;
                            }
                        }

                        ConfigText storedKey = std::holds_alternative<std::u16string_view>(key) ? ConfigText(std::get<std::u16string_view>(key)) : ConfigText(std::move(std::get<std::u16string>(key))); // 新字段自有 key
                        object.push_back(ConfigObjectEntry{ std::move(storedKey), std::move(value) });
                    }

                    static void AppendUnsignedDecimal(std::u16string& output, size_t value) {
                        std::array<char, std::numeric_limits<size_t>::digits10 + 1> digits{}; // 十进制最大位数
                        const auto converted = std::to_chars(digits.data(), digits.data() + digits.size(), value);
                        for (const char* digit = digits.data(); digit != converted.ptr; ++digit) output.push_back(static_cast<char16_t>(*digit));
                    }

                    // 扫描失败前缀得到自包含行列，避免 Result 借用输入文本生命周期。
                    static void MeasureParsePosition(
                        const char16_t* text, size_t textLength, size_t position,
                        size_t& line, size_t& column) noexcept {
                        line = 1;
                        column = 1;
                        const size_t limit = std::min(position, textLength); // 防止失败游标越过文本尾部
                        for (size_t i = 0; i < limit; ++i) {
                            if (text[i] == u'\n') {
                                ++line;
                                column = 1;
                            } else {
                                // 合法 surrogate pair 在用户可见列中只计一个 code point。
                                if (text[i] >= 0xD800 && text[i] <= 0xDBFF && i + 1 < limit && text[i + 1] >= 0xDC00 && text[i + 1] <= 0xDFFF) ++i;
                                ++column;
                            }
                        }
                    }

                    // GetStatus 才组装诊断字符串；IsOk 失败探测不支付分配成本。
                    static Status MaterializeParseStatus(
                        size_t line, size_t column, const char16_t* message) {
                        const std::u16string_view messageView(message == nullptr ? u"" : message);
                        std::u16string diagnostic; // 单缓冲组装，避免失败路径进入通用格式化器
                        diagnostic.reserve(messageView.size() + 48U);
                        diagnostic.append(u"JSON parse error at line ");
                        AppendUnsignedDecimal(diagnostic, line);
                        diagnostic.append(u", column ");
                        AppendUnsignedDecimal(diagnostic, column);
                        diagnostic.append(u": ");
                        diagnostic.append(messageView);
                        return Status::InvalidArgument(String(std::u16string_view(diagnostic)));
                    }

                    // 语法失败只记录游标与静态短语；诊断文本延迟到 GetStatus。
                    Result<ConfigValue> Error(const char16_t* message, size_t position) const {
                        size_t line = 1; // 从 1 开始的失败行
                        size_t column = 1; // 按 Unicode code point 计数的失败列
                        MeasureParsePosition(m_text.data(), m_text.size(), position, line, column);
                        return Result<ConfigValue>::LazyFailure(&MaterializeParseStatus, line, column, message);
                    }

                    // 未预期异常仍立即物化，避免依赖临时 std::string 生命周期。
                    Result<ConfigValue> Error(const String& message, size_t position) const {
                        size_t line = 1; // 从 1 开始的失败行
                        size_t column = 1; // 按 Unicode code point 计数的失败列
                        MeasureParsePosition(m_text.data(), m_text.size(), position, line, column);
                        return MaterializeParseStatus(line, column, message.data());
                    }

                    Result<ConfigValue> TrailingDataError() const {
                        if (m_hasError) return Error(m_errorMessage, m_pos);
                        return Error(u"unexpected data after JSON value", m_pos);
                    }

                    // 记录首个语法错误；递归回退不得覆盖更具体的底层诊断。
                    void Fail(const char16_t* message) noexcept {
                        if (m_hasError) return;
                        m_errorMessage = message;
                        m_hasError = true;
                    }

                    // JSON5 空白包括 ASCII 空白、BOM 和 JavaScript 行/段分隔符。
                    void SkipWhitespace() {
                        while (m_pos < m_text.size()) {
                            const char16_t value = m_text[m_pos]; // 当前候选空白或注释起点
                            if (value == u' ' || (value >= u'\t' && value <= u'\r')) {
                                ++m_pos;
                                continue;
                            }
                            if (value != u'/' && value < 0x00A0) return;
                            SkipJson5WhitespaceSlow();
                            return;
                        }
                    }

                    // 注释和非 ASCII 空白属于稀有路径，避免其循环阻碍普通 JSON 空白扫描内联。
                    LP_CONFIG_NOINLINE void SkipJson5WhitespaceSlow() {
                        while (m_pos < m_text.size()) {
                            const char16_t value = m_text[m_pos]; // JSON5 扩展空白或注释起点
                            if (value == u' ' || (value >= u'\t' && value <= u'\r')) {
                                ++m_pos;
                                continue;
                            }
                            if (value >= 0x00A0 && IsJson5ExtendedSpace(value)) {
                                ++m_pos;
                                continue;
                            }
                            if (value != u'/' || m_pos + 1 >= m_text.size()) return;
                            const char16_t commentKind = m_text[m_pos + 1]; // 行或块注释标记
                            if (commentKind == u'/') {
                                m_pos += 2;
                                while (m_pos < m_text.size() && m_text[m_pos] != u'\r' && m_text[m_pos] != u'\n' && m_text[m_pos] != 0x2028 && m_text[m_pos] != 0x2029) ++m_pos;
                                continue;
                            }
                            if (m_text[m_pos + 1] == u'*') {
                                m_pos += 2;
                                while (m_pos + 1 < m_text.size() && !(m_text[m_pos] == u'*' && m_text[m_pos + 1] == u'/')) ++m_pos;
                                if (m_pos + 1 >= m_text.size()) {
                                    Fail(u"unterminated comment");
                                    m_pos = m_text.size() + 1U; // 与合法文档末尾区分，由根冷出口保留诊断
                                    return;
                                }
                                m_pos += 2;
                                continue;
                            }
                            return;
                        }
                    }

                    static bool IsJson5Space(char16_t value) noexcept {
                        return value == u' ' || (value >= u'\t' && value <= u'\r') || IsJson5ExtendedSpace(value);
                    }

                    static bool IsJson5ExtendedSpace(char16_t value) noexcept {
                        return value == 0x00A0 || value == 0x1680 || (value >= 0x2000 && value <= 0x200A) || value == 0x2028 || value == 0x2029 || value == 0x202F || value == 0x205F || value == 0x3000 || value == 0xFEFF;
                    }

                    static bool IsJson5Quote(char16_t value) noexcept {
                        return value == u'"' || value == u'\'';
                    }

                    static bool IsJsonDigit(char16_t value) noexcept {
                        return value >= u'0' && value <= u'9';
                    }

                    // 大数组预留可避免多次搬移子节点指针；对象不走该扫描以免深层结构退化。
                    size_t EstimateArrayItems() const {
                        size_t position = m_pos; // 当前数组首元素位置
                        while (position < m_text.size() && IsJson5Space(m_text[position])) ++position;
                        if (position >= m_text.size() || m_text[position] == u']') return 0;

                        size_t count = 1; // 非空数组至少一个顶层元素
                        size_t depth = 0; // 内嵌对象或数组深度
                        bool inString = false; // 严格 JSON 双引号字符串状态
                        bool escaped = false; // 字符串转义状态
                        for (; position < m_text.size(); ++position) {
                            const char16_t value = m_text[position]; // 当前扫描 UTF-16 单元
                            if (inString) {
                                if (escaped) escaped = false;
                                else if (value == u'\\') escaped = true;
                                else if (value == u'"') inString = false;
                                continue;
                            }
                            if (value == u'"') inString = true;
                            else if (value == u'\'' || value == u'/') return 0; // JSON5 扩展放弃预留
                            else if (value == u'{' || value == u'[') ++depth;
                            else if (value == u'}' || value == u']') {
                                if (depth == 0 && value == u']') break;
                                if (depth > 0) --depth;
                            } else if (value == u',' && depth == 0) ++count;
                        }
                        return count;
                    }

                    // 匹配 true/false/null 等固定字面量，成功后推进游标。
                    bool MatchLiteral(const char16_t* literal, size_t length) {
                        if (m_pos + length > m_text.size()) return false;
                        for (size_t i = 0; i < length; ++i) if (m_text[m_pos + i] != literal[i]) return false;
                        m_pos += length;
                        return true;
                    }

                    static bool IsIdentifierStart(char16_t value) noexcept {
                        return (value >= u'a' && value <= u'z') || (value >= u'A' && value <= u'Z') || value == u'_' || value == u'$' || value >= 0x80;
                    }

                    static bool IsIdentifierPart(char16_t value) noexcept {
                        return IsIdentifierStart(value) || IsJsonDigit(value);
                    }

                    // JSON5 对象键允许 IdentifierName，不要求每个键都写引号。
                    std::optional<ParsedString> ParseIdentifierKey(bool allowTrailingClose = false) {
                        if (m_pos >= m_text.size() || (m_text[m_pos] != u'\\' &&
                            !IsIdentifierStart(m_text[m_pos]))) {
                            if (allowTrailingClose && m_pos < m_text.size() && m_text[m_pos] == u'}') return std::nullopt;
                            Fail(u"expected object key string or identifier");
                            return std::nullopt;
                        }
                        const size_t begin = m_pos; // 无转义裸 key 的借用起点
                        bool first = true;
                        std::u16string decoded; // 遇到 Unicode 转义后才启用的拥有型 key
                        while (m_pos < m_text.size()) {
                            char16_t value = m_text[m_pos];
                            if (value == u'\\') {
                                if (decoded.empty()) decoded.assign(m_text.data() + begin, m_pos - begin);
                                if (m_pos + 1 >= m_text.size() || m_text[m_pos + 1] != u'u') {
                                    Fail(u"invalid identifier escape");
                                    return std::nullopt;
                                }
                                m_pos += 2;
                                auto escaped = ParseHex4();
                                if (!escaped) return std::nullopt;
                                value = *escaped;
                                if ((first && !IsIdentifierStart(value)) || (!first && !IsIdentifierPart(value))) {
                                    Fail(u"invalid escaped identifier character");
                                    return std::nullopt;
                                }
                                decoded.push_back(value);
                                first = false;
                                continue;
                            }
                            if ((first && !IsIdentifierStart(value)) || (!first && !IsIdentifierPart(value))) break;
                            if (!decoded.empty()) decoded.push_back(value);
                            ++m_pos;
                            first = false;
                        }
                        if (first) {
                            Fail(u"expected object key string or identifier");
                            return std::nullopt;
                        }
                        if (!decoded.empty()) {
                            return std::optional<ParsedString>(std::in_place, std::in_place_index<1>, std::move(decoded));
                        }
                        return std::optional<ParsedString>(std::in_place, std::in_place_index<0>, std::u16string_view(m_text.data() + begin, m_pos - begin));
                    }

                    // 按首字符分派 JSON value，所有分支都消费完整值。
                    std::optional<ConfigNode> ParseValue() {
                        SkipWhitespace();
                        return ParseValueAfterWhitespace();
                    }

                    // 在调用方已完成前导空白扫描时直接分派，避免容器元素重复扫描。
                    std::optional<ConfigNode> ParseValueAfterWhitespace(bool allowTrailingClose = false) {
                        if (m_pos >= m_text.size()) {
                            Fail(u"unexpected end of input");
                            return std::nullopt;
                        }

                        char16_t ch = m_text[m_pos]; // 当前 value 的首字符，用于选择解析分支
                        if (ch == U'{') return ParseObject();
                        if (ch == U'[') return ParseArray();
                        if (ch == U'"') {
                            auto text = ParseString<u'"'>(); // 严格 JSON 双引号保持最短分支
                            if (!text) return std::nullopt;
                            return MakeParsedStringValue(std::move(*text));
                        }
                        // 固定字面量必须完整匹配，不能接受 truex 这类前缀误判。
                        if (ch == U't') {
                            if (!MatchLiteral(u"true", 4)) {
                                Fail(u"invalid literal");
                                return std::nullopt;
                            }
                            return ConfigNode(true);
                        }
                        if (ch == U'f') {
                            if (!MatchLiteral(u"false", 5)) {
                                Fail(u"invalid literal");
                                return std::nullopt;
                            }
                            return ConfigNode(false);
                        }
                        if (ch == U'n') {
                            if (!MatchLiteral(u"null", 4)) {
                                Fail(u"invalid literal");
                                return std::nullopt;
                            }
                            return ConfigNode();
                        }
                        if (ch == U'-' || IsJsonDigit(ch)) return ParseNumber();
                        if (ch == U'\'') {
                            auto text = ParseString<u'\''>(); // JSON5 单引号字符串
                            if (!text) return std::nullopt;
                            return MakeParsedStringValue(std::move(*text));
                        }
                        if (ch == U'+' || ch == U'.' || ch == U'I' || ch == U'N') return ParseNumber();
                        if (allowTrailingClose && ch == U']') return std::nullopt;

                        Fail(u"invalid JSON value");
                        return std::nullopt;
                    }

                    // 解析对象，重复字段在内部有序表中覆盖旧节点。
                    std::optional<ConfigNode> ParseObject() {
                        ++m_pos;
                        ConfigObject object; // 当前对象字段按出现顺序保存
                        object.reserve(4); // 服务配置对象通常字段较少，避免深层对象反复向后预扫描

                        SkipWhitespace();
                        // 空对象是合法对象，直接消费右花括号。
                        if (m_pos < m_text.size() && m_text[m_pos] == U'}') {
                            ++m_pos;
                            return ConfigNode(std::move(object));
                        }

                        bool afterComma = false; // true 时下一轮允许 JSON5 尾逗号直接闭合对象
                        while (true) {
                            if (m_pos >= m_text.size()) {
                                Fail(u"expected object key string or identifier");
                                return std::nullopt;
                            }

                            auto key = m_text[m_pos] == u'"' ? ParseString<u'"'>() : (m_text[m_pos] == u'\'' ? ParseString<u'\''>() : ParseIdentifierKey(afterComma));
                            if (!key) {
                                if (afterComma && m_pos < m_text.size() && m_text[m_pos] == U'}') {
                                    ++m_pos;
                                    break;
                                }
                                return std::nullopt;
                            }
                            afterComma = false;
                            SkipWhitespace();
                            if (m_pos >= m_text.size() || m_text[m_pos] != U':') {
                                Fail(u"expected ':' after object key");
                                return std::nullopt;
                            }
                            ++m_pos;

                            // value 可递归为对象/数组；Set 会保持对象存储语义。
                            auto value = ParseValue(); // 当前字段值或递归失败状态
                            if (!value) return std::nullopt;
                            SetParsedEntry(object, std::move(*key), std::move(*value));
                            SkipWhitespace();

                            if (m_pos >= m_text.size()) {
                                Fail(u"unexpected end in object");
                                return std::nullopt;
                            }
                            if (m_text[m_pos] == U'}') {
                                ++m_pos;
                                break;
                            }
                            if (m_text[m_pos] != U',') {
                                Fail(u"expected ',' between object fields");
                                return std::nullopt;
                            }
                            ++m_pos;
                            SkipWhitespace();
                            afterComma = true;
                        }

                        return ConfigNode(std::move(object));
                    }

                    // 解析数组，元素按出现顺序追加到 ConfigArray。
                    std::optional<ConfigNode> ParseArray() {
                        ++m_pos;
                        ConfigArray array; // 当前数组节点，保留元素顺序
                        array.reserve(EstimateArrayItems());

                        SkipWhitespace();
                        // 空数组直接消费右中括号。
                        if (m_pos < m_text.size() && m_text[m_pos] == U']') {
                            ++m_pos;
                            return ConfigNode(std::move(array));
                        }

                        bool afterComma = false; // true 时下一轮允许 JSON5 尾逗号直接闭合数组
                        while (true) {
                            auto value = ParseValueAfterWhitespace(afterComma); // 前导空白已由容器消费
                            if (!value) {
                                if (afterComma && m_pos < m_text.size() && m_text[m_pos] == U']') {
                                    ++m_pos;
                                    break;
                                }
                                return std::nullopt;
                            }
                            afterComma = false;
                            array.push_back(std::move(*value));
                            SkipWhitespace();

                            if (m_pos >= m_text.size()) {
                                Fail(u"unexpected end in array");
                                return std::nullopt;
                            }
                            if (m_text[m_pos] == U']') {
                                ++m_pos;
                                break;
                            }
                            if (m_text[m_pos] != U',') {
                                Fail(u"expected ',' between array values");
                                return std::nullopt;
                            }
                            ++m_pos;
                            SkipWhitespace();
                            afterComma = true;
                        }

                        return ConfigNode(std::move(array));
                    }

                    // 解析两位十六进制 payload，用于 JSON5 的 \xXX 字符串转义。
                    std::optional<char16_t> ParseHex2() {
                        if (m_pos + 2 > m_text.size()) {
                            Fail(u"incomplete hexadecimal escape");
                            return std::nullopt;
                        }
                        const char16_t high = m_text[m_pos++];
                        const char16_t low = m_text[m_pos++];
                        if (!IsHexDigit(high) || !IsHexDigit(low)) {
                            Fail(u"invalid hexadecimal escape digit");
                            return std::nullopt;
                        }
                        return static_cast<char16_t>((HexValue(high) << 4) + HexValue(low));
                    }

                    // 解析 \uXXXX 的四位十六进制 payload。
                    std::optional<char16_t> ParseHex4() {
                        if (m_pos + 4 > m_text.size()) {
                            Fail(u"incomplete unicode escape");
                            return std::nullopt;
                        }

                        char16_t value = 0; // 当前累计的 Unicode 转义数值
                        for (int i = 0; i < 4; ++i) {
                            char16_t ch = m_text[m_pos++]; // 当前十六进制字符
                            if (!IsHexDigit(ch)) {
                                Fail(u"invalid unicode escape digit");
                                return std::nullopt;
                            }
                            value = static_cast<char16_t>((value << 4) + HexValue(ch));
                        }
                        return value;
                    }

#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 15
#pragma GCC diagnostic push
                    // GCC 15 ASan -O2 会把 inactive variant string 分支误报为未初始化。
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
                    // 解析 JSON5 字符串，输出为已解码的 Unicode 文本。
                    template <char16_t Quote>
                    std::optional<ParsedString> ParseString() {
                        if (m_text[m_pos] != Quote) {
                            Fail(u"expected string");
                            return std::nullopt;
                        }
                        ++m_pos;

                        const size_t begin = m_pos; // 无转义快路径的字符串起点
                        while (m_pos < m_text.size()) {
                            const char16_t codeUnit = m_text[m_pos]; // 尚未消费的原始 UTF-16 单元
                            if (codeUnit == Quote) {
                                const auto view = std::u16string_view(m_text.data() + begin, m_pos - begin);
                                ++m_pos;
                                return std::optional<ParsedString>(std::in_place, std::in_place_index<0>, view);
                            }
                            if (codeUnit < 0x20) {
                                Fail(u"control character in string");
                                return std::nullopt;
                            }
                            if (codeUnit == u'\\') break;
                            if (codeUnit >= 0x2028) {
                                if (codeUnit <= 0x2029) {
                                    Fail(u"control character in string");
                                    return std::nullopt;
                                }
                                if (codeUnit >= 0xD800 && codeUnit <= 0xDFFF) break;
                            }
                            ++m_pos;
                        }

                        std::u16string output(m_text.data() + begin, m_text.data() + m_pos); // 转义前缀
                        output.reserve(output.size() + 16);
                        while (m_pos < m_text.size()) {
                            char16_t ch = m_text[m_pos++]; // 当前原始字符串 code unit
                            if (ch == Quote) return std::optional<ParsedString>(std::in_place, std::in_place_index<1>, std::move(output));

                            if (ch < 0x20 || (ch >= 0x2028 && ch <= 0x2029)) {
                                Fail(u"control character in string");
                                return std::nullopt;
                            }
                            if (ch != u'\\') {
                                if (ch >= 0xD800 && ch <= 0xDBFF) {
                                    if (m_pos >= m_text.size() || m_text[m_pos] < 0xDC00 || m_text[m_pos] > 0xDFFF) {
                                        Fail(u"expected low surrogate after high surrogate");
                                        return std::nullopt;
                                    }
                                    output.push_back(ch);
                                    output.push_back(m_text[m_pos++]);
                                    continue;
                                }
                                if (ch >= 0xDC00 && ch <= 0xDFFF) {
                                    Fail(u"low surrogate without high surrogate");
                                    return std::nullopt;
                                }
                                output.push_back(ch);
                                continue;
                            }

                            if (m_pos >= m_text.size()) {
                                Fail(u"incomplete escape sequence");
                                return std::nullopt;
                            }
                            char16_t escaped = m_text[m_pos++]; // 转义类型字符
                            switch (escaped) {
                                case u'"': output.push_back(u'"'); break;
                                case u'\'': output.push_back(u'\''); break;
                                case u'\\': output.push_back(u'\\'); break;
                                case u'/': output.push_back(u'/'); break;
                                case u'b': output.push_back(u'\b'); break;
                                case u'f': output.push_back(u'\f'); break;
                                case u'n': output.push_back(u'\n'); break;
                                case u'r': output.push_back(u'\r'); break;
                                case u't': output.push_back(u'\t'); break;
                                case u'v': output.push_back(u'\v'); break;
                                case u'0':
                                    if (m_pos < m_text.size() && IsJsonDigit(m_text[m_pos])) {
                                        Fail(u"invalid null escape");
                                        return std::nullopt;
                                    }
                                    output.push_back(u'\0');
                                    break;
                                case u'x': {
                                    auto value = ParseHex2();
                                    if (!value) return std::nullopt;
                                    output.push_back(*value);
                                    break;
                                }
                                case u'\r':
                                    if (m_pos < m_text.size() && m_text[m_pos] == u'\n') ++m_pos;
                                    break;
                                case u'\n':
                                case 0x2028:
                                case 0x2029: break;
                                case u'u': {
                                    auto highResult = ParseHex4(); // 高位或普通 Unicode 转义值
                                    if (!highResult) return std::nullopt;
                                    const char16_t high = *highResult; // 已验证的高位或普通转义值
                                    // JSON surrogate pair 必须成对出现，内部直接保留合法 UTF-16 单元。
                                    if (high >= 0xD800 && high <= 0xDBFF) {
                                        if (m_pos + 2 > m_text.size() ||
                                            m_text[m_pos] != u'\\' || m_text[m_pos + 1] != u'u') {
                                            Fail(u"expected low surrogate after high surrogate");
                                            return std::nullopt;
                                        }
                                        m_pos += 2;
                                        auto lowResult = ParseHex4(); // 低代理转义值或失败状态
                                        if (!lowResult) return std::nullopt;
                                        const char16_t low = *lowResult; // 已解析的低代理值
                                        if (low < 0xDC00 || low > 0xDFFF) {
                                            Fail(u"invalid low surrogate");
                                            return std::nullopt;
                                        }
                                        output.push_back(high);
                                        output.push_back(low);
                                    } else if (high >= 0xDC00 && high <= 0xDFFF) {
                                        // 单独低代理不是合法 Unicode scalar，直接拒绝。
                                        Fail(u"low surrogate without high surrogate");
                                        return std::nullopt;
                                    } else output.push_back(high);
                                    break;
                                }
                                // JSON5 保留 JavaScript 的非特殊字符转义语义，例如 \a -> a。
                                default: output.push_back(escaped); break;
                            }
                        }

                        Fail(u"unterminated string");
                        return std::nullopt;
                    }
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 15
#pragma GCC diagnostic pop
#endif

                    // 解析 JSON5 number，并按是否含小数/指数选择 int64 或 double。
                    std::optional<ConfigNode> ParseNumber() {
                        const size_t begin = m_pos;
                        // 常见正整数只扫描和转换一次，JSON5 扩展前缀继续走下方完整路径。
                        if (m_text[m_pos] >= u'1' && m_text[m_pos] <= u'9') {
                            while (m_pos < m_text.size() && IsJsonDigit(m_text[m_pos])) ++m_pos;
                            if (m_pos >= m_text.size() || (m_text[m_pos] != u'.' && m_text[m_pos] != u'e' && m_text[m_pos] != u'E')) {
                                uint64_t magnitude = 0; // 正整数无符号幅值
                                const uint64_t limit = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
                                for (size_t i = begin; i < m_pos; ++i) {
                                    const uint64_t digit = static_cast<uint64_t>(m_text[i] - u'0');
                                    if (magnitude > (limit - digit) / 10U) {
                                        Fail(u"integer number is out of int64 range");
                                        return std::nullopt;
                                    }
                                    magnitude = magnitude * 10U + digit;
                                }
                                return ConfigNode(static_cast<int64_t>(magnitude));
                            }
                            m_pos = begin;
                        } else if (m_text[m_pos] == u'0' && (m_pos + 1 >= m_text.size() || (
                            m_text[m_pos + 1] != u'.' &&
                            m_text[m_pos + 1] != u'e' &&
                            m_text[m_pos + 1] != u'E' &&
                            m_text[m_pos + 1] != u'x' &&
                            m_text[m_pos + 1] != u'X' &&
                            !IsJsonDigit(m_text[m_pos + 1])
                        ))) {
                            ++m_pos;
                            return ConfigNode(int64_t{ 0 });
                        }
                        bool negative = false;
                        if (m_text[m_pos] == U'+' || m_text[m_pos] == U'-') {
                            negative = m_text[m_pos] == U'-';
                            ++m_pos;
                        }
                        if (m_pos >= m_text.size()) {
                            Fail(u"incomplete number");
                            return std::nullopt;
                        }

                        auto matchSpecial = [&](const char16_t* literal, size_t length) {
                            if (m_pos + length > m_text.size()) return false;
                            for (size_t i = 0; i < length; ++i) if (m_text[m_pos + i] != literal[i]) return false;
                            m_pos += length;
                            return true;
                        };
                        if (m_text[m_pos] >= u'A') {
                            if (m_text[m_pos] == u'I' && matchSpecial(u"Infinity", 8)) return ConfigNode(negative ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity());
                            if (m_text[m_pos] != u'N' || !matchSpecial(u"NaN", 3)) {
                                Fail(u"invalid numeric literal");
                                return std::nullopt;
                            }
                            return ConfigNode(std::numeric_limits<double>::quiet_NaN());
                        }

                        // JSON5 十六进制整数允许可选正负号。
                        if (m_text[m_pos] == u'0' && m_pos + 2 <= m_text.size() && (m_text[m_pos + 1] == u'x' || m_text[m_pos + 1] == u'X')) {
                            m_pos += 2;
                            const size_t digitsBegin = m_pos;
                            while (m_pos < m_text.size() && IsHexDigit(m_text[m_pos])) ++m_pos;
                            if (m_pos == digitsBegin) {
                                Fail(u"expected hexadecimal digit");
                                return std::nullopt;
                            }
                            const uint64_t limit = negative ? static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1U : static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
                            uint64_t magnitude = 0;
                            for (size_t i = digitsBegin; i < m_pos; ++i) {
                                const uint64_t digit = static_cast<uint64_t>(HexValue(m_text[i]));
                                if (magnitude > (limit - digit) / 16U) {
                                    Fail(u"integer number is out of int64 range");
                                    return std::nullopt;
                                }
                                magnitude = magnitude * 16U + digit;
                            }
                            return ConfigNode(negative ? (magnitude == limit ? std::numeric_limits<int64_t>::min() : -static_cast<int64_t>(magnitude)) : static_cast<int64_t>(magnitude));
                        }

                        bool isFloat = false;
                        bool hasIntegerDigits = false;
                        if (m_pos < m_text.size() && m_text[m_pos] == u'.') isFloat = true;
                        else if (m_pos < m_text.size() && IsJsonDigit(m_text[m_pos])) {
                            hasIntegerDigits = true;
                            if (m_text[m_pos] == u'0') {
                                ++m_pos;
                                if (m_pos < m_text.size() && IsJsonDigit(m_text[m_pos])) {
                                    Fail(u"leading zero is not allowed");
                                    return std::nullopt;
                                }
                            } else while (m_pos < m_text.size() && IsJsonDigit(m_text[m_pos])) ++m_pos;
                        } else {
                            Fail(u"expected digit in number");
                            return std::nullopt;
                        }

                        if (m_pos < m_text.size() && m_text[m_pos] == u'.') {
                            isFloat = true;
                            ++m_pos;
                            const size_t fractionBegin = m_pos;
                            while (m_pos < m_text.size() && IsJsonDigit(m_text[m_pos])) ++m_pos;
                            if (!hasIntegerDigits && m_pos == fractionBegin) {
                                Fail(u"expected digit after decimal point");
                                return std::nullopt;
                            }
                        }

                        if (m_pos < m_text.size() && (m_text[m_pos] == u'e' || m_text[m_pos] == u'E')) {
                            isFloat = true;
                            ++m_pos;
                            if (m_pos < m_text.size() && (m_text[m_pos] == u'+' || m_text[m_pos] == u'-')) ++m_pos;
                            if (m_pos >= m_text.size() || !IsJsonDigit(m_text[m_pos])) {
                                Fail(u"expected exponent digit");
                                return std::nullopt;
                            }
                            while (m_pos < m_text.size() && IsJsonDigit(m_text[m_pos])) ++m_pos;
                        }

                        if (isFloat) {
                            std::string number;
                            number.reserve(m_pos - begin);
                            for (size_t i = begin; i < m_pos; ++i) number.push_back(static_cast<char>(m_text[i]));
                            char* parsedEnd = nullptr;
                            const double value = std::strtod(number.c_str(), &parsedEnd);
                            if (parsedEnd != number.data() + number.size() || !std::isfinite(value)) {
                                Fail(u"invalid floating number");
                                return std::nullopt;
                            }
                            return ConfigNode(value);
                        }

                        const uint64_t limit = negative ? static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1U : static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
                        uint64_t magnitude = 0;
                        const size_t digitsBegin = begin + ((m_text[begin] == u'+' || m_text[begin] == u'-') ? 1U : 0U);
                        for (size_t i = digitsBegin; i < m_pos; ++i) {
                            const uint64_t digit = static_cast<uint64_t>(m_text[i] - u'0');
                            if (magnitude > (limit - digit) / 10U) {
                                Fail(u"integer number is out of int64 range");
                                return std::nullopt;
                            }
                            magnitude = magnitude * 10U + digit;
                        }
                        return ConfigNode(negative ? (magnitude == limit ? std::numeric_limits<int64_t>::min() : -static_cast<int64_t>(magnitude)) : static_cast<int64_t>(magnitude));
                    }

                    std::u16string_view m_text; // 同步解析期间借用调用方 Core/String 的原生 UTF-16 存储
                    size_t m_pos = 0;      // 当前解析位置，单位为 UTF-16 code unit
                    const char16_t* m_errorMessage = u"invalid JSON5"; // 首个普通语法失败消息
                    bool m_hasError = false; // 保留最内层诊断，避免递归回退覆盖
                };
            }

#undef LP_CONFIG_NOINLINE

            // 包内 JSON5 解析入口，隐藏具体递归下降实现。
            Result<ConfigValue> ParseJsonDocument(const String& text) {
                JsonParser parser(text); // 单次解析器，持有本次文档和游标
                return parser.Parse();
            }

            namespace {
                // 饱和加法，避免浅层预留下溢回绕。
                size_t SaturatingAdd(size_t left, size_t right) noexcept {
                    const size_t sum = left + right; // 可能回绕的候选和
                    return sum < left ? (std::numeric_limits<size_t>::max)() : sum;
                }

                // 饱和乘法保护容器元素数与经验字节因子的组合。
                size_t SaturatingMultiply(size_t value, size_t factor) noexcept {
                    const size_t limit = (std::numeric_limits<size_t>::max)(); // size_t 上界
                    return factor != 0 && value > limit / factor ? limit : value * factor;
                }

                // 只看根下一层广度猜测紧凑容量：服务大数组，避免深层薄树二次全遍历。
                size_t GuessCompactJsonUtf16Capacity(const ConfigNode& value) noexcept {
                    const auto& storage = value.m_value; // 与序列化同一分派面
                    size_t guess = 4096; // 与历史紧凑下限对齐
                    if (storage.index() == 5) {
                        const auto& array = std::get<ConfigArray>(storage); // 根数组
                        guess = SaturatingAdd(guess, SaturatingAdd(SaturatingMultiply(array.size(), 96), 16));
                        return guess;
                    }
                    if (storage.index() != 6) return guess;

                    const auto& object = std::get<ConfigObject>(storage); // 根对象
                    for (const auto& entry : object) {
                        guess = SaturatingAdd(guess, SaturatingAdd(entry.key.View().size(), 16));
                        const auto& child = entry.value.m_value; // 仅展开一层子节点
                        if (child.index() == 5) {
                            const auto& array = std::get<ConfigArray>(child); // 常见 items 大数组
                            guess = SaturatingAdd(guess, SaturatingAdd(SaturatingMultiply(array.size(), 96), 16));
                        } else if (child.index() == 6) {
                            const auto& nested = std::get<ConfigObject>(child); // 浅对象
                            guess = SaturatingAdd(guess, SaturatingAdd(SaturatingMultiply(nested.size(), 64), 16));
                        } else guess = SaturatingAdd(guess, 32);
                    }
                    return guess;
                }

                // JSON5 字符串以双引号安全形式写入；无转义连续段一次 append，避免逐码元 push_back。
                void AppendQuotedJsonString(std::u16string& output, std::u16string_view value) {
                    output.push_back(u'"');
                    const char16_t* data = value.data(); // 输入 UTF-16 起点
                    const size_t length = value.size(); // 输入码元数
                    size_t index = 0; // 当前扫描位置
                    while (index < length) {
                        const size_t runBegin = index; // 当前无转义段起点
                        while (index < length) {
                            const char16_t codeUnit = data[index]; // 当前码元
                            // 双引号、反斜杠和控制字符必须进入转义慢路径。
                            if (codeUnit == u'"' || codeUnit == u'\\' || codeUnit < 0x20) break;
                            ++index;
                        }

                        // 整段无转义文本一次写入。
                        if (index > runBegin) output.append(data + runBegin, index - runBegin);
                        if (index >= length) break;

                        const char16_t codeUnit = data[index++]; // 需要转义的单个码元
                        switch (codeUnit) {
                            case u'"': output.append(u"\\\""); break;
                            case u'\\': output.append(u"\\\\"); break;
                            case u'\b': output.append(u"\\b"); break;
                            case u'\f': output.append(u"\\f"); break;
                            case u'\n': output.append(u"\\n"); break;
                            case u'\r': output.append(u"\\r"); break;
                            case u'\t': output.append(u"\\t"); break;
                            default: {
                                static constexpr char16_t hex[] = u"0123456789ABCDEF";
                                output.append(u"\\u");
                                output.push_back(hex[(codeUnit >> 12) & 0xF]);
                                output.push_back(hex[(codeUnit >> 8) & 0xF]);
                                output.push_back(hex[(codeUnit >> 4) & 0xF]);
                                output.push_back(hex[codeUnit & 0xF]);
                                break;
                            }
                        }
                    }
                    output.push_back(u'"');
                }

                // to_chars 先输出 ASCII，再一次性抬升为 UTF-16，避免逐字节 push_back。
                template <typename Number>
                void AppendNumber(std::u16string& output, Number value) {
                    char buffer[64]; // int64/double 的最大稳定文本远小于该缓冲
                    std::to_chars_result result;
                    if constexpr (std::is_floating_point_v<Number>) {
                        // JSON5 保留非有限数；严格 JSON 输入仍可无损往返普通有限数。
                        if (std::isnan(value)) {
                            output.append(u"NaN");
                            return;
                        }
                        if (std::isinf(value)) {
                            output.append(value < 0 ? u"-Infinity" : u"Infinity");
                            return;
                        }
                        result = std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::general, std::numeric_limits<Number>::max_digits10);
                    } else result = std::to_chars(buffer, buffer + sizeof(buffer), value);
                    if (result.ec != std::errc()) throw std::runtime_error("JSON number formatting failed");

                    const size_t asciiLength = static_cast<size_t>(result.ptr - buffer); // ASCII 数字长度
                    char16_t utf16[64]; // 同长度 UTF-16 暂存
                    for (size_t i = 0; i < asciiLength; ++i) utf16[i] = static_cast<char16_t>(static_cast<unsigned char>(buffer[i]));
                    output.append(utf16, asciiLength);
                }

                // 紧凑容器写入前置声明，供叶子快路径回退调用。
                void SerializeJsonValueUtf16Compact(const ConfigNode& value, std::u16string& output);

                // 紧凑叶子写入，避免小对象字段再进入一次递归调用。
                void SerializeCompactLeafOrNode(const ConfigNode& value, std::u16string& output) {
                    const auto& storage = value.m_value; // 当前节点存储
                    switch (storage.index()) {
                        case 0:
                            output.append(u"null");
                            return;
                        case 1:
                            AppendQuotedJsonString(output, std::get<ConfigText>(storage).View());
                            return;
                        case 2:
                            AppendNumber(output, std::get<int64_t>(storage));
                            return;
                        case 3:
                            AppendNumber(output, std::get<double>(storage));
                            return;
                        case 4:
                            output.append(std::get<bool>(storage) ? u"true" : u"false");
                            return;
                        default:
                            // 容器继续走完整紧凑递归。
                            SerializeJsonValueUtf16Compact(value, output);
                            return;
                    }
                }

                // 紧凑输出专用：无缩进分支，服务 fair/默认 ToJson(-1) 热路径。
                void SerializeJsonValueUtf16Compact(const ConfigNode& value, std::u16string& output) {
                    const auto& storage = value.m_value; // 单次分派内部 variant
                    switch (storage.index()) {
                        case 0:
                            output.append(u"null");
                            return;
                        case 1:
                            AppendQuotedJsonString(output, std::get<ConfigText>(storage).View());
                            return;
                        case 2:
                            AppendNumber(output, std::get<int64_t>(storage));
                            return;
                        case 3:
                            AppendNumber(output, std::get<double>(storage));
                            return;
                        case 4:
                            output.append(std::get<bool>(storage) ? u"true" : u"false");
                            return;
                        case 5: {
                            const auto& array = std::get<ConfigArray>(storage); // 按数组顺序递归写入
                            output.push_back(u'[');
                            for (size_t i = 0; i < array.size(); ++i) {
                                if (i > 0) output.push_back(u',');
                                SerializeCompactLeafOrNode(array[i], output);
                            }
                            output.push_back(u']');
                            return;
                        }
                        case 6: {
                            const auto& object = std::get<ConfigObject>(storage); // 保持字段插入顺序
                            output.push_back(u'{');
                            for (size_t i = 0; i < object.size(); ++i) {
                                if (i > 0) output.push_back(u',');
                                AppendQuotedJsonString(output, object[i].key.View());
                                output.push_back(u':');
                                SerializeCompactLeafOrNode(object[i].value, output);
                            }
                            output.push_back(u'}');
                            return;
                        }
                        default: output.append(u"null"); return;
                    }
                }

                void SerializeJsonValueUtf16(const ConfigNode& value, std::u16string& output, int indent, int level) {
                    const auto& storage = value.m_value; // 单次分派内部 variant，不包装公开 PImpl
                    switch (storage.index()) {
                        case 0:
                            output.append(u"null");
                            return;
                        case 1:
                            AppendQuotedJsonString(output, std::get<ConfigText>(storage).View());
                            return;
                        case 2:
                            AppendNumber(output, std::get<int64_t>(storage));
                            return;
                        case 3:
                            AppendNumber(output, std::get<double>(storage));
                            return;
                        case 4:
                            output.append(std::get<bool>(storage) ? u"true" : u"false");
                            return;
                        case 5: {
                            const auto& array = std::get<ConfigArray>(storage); // 按数组顺序递归写入
                            output.push_back(u'[');
                            for (size_t i = 0; i < array.size(); ++i) {
                                if (i > 0) output.push_back(u',');
                                if (indent >= 0) {
                                    output.push_back(u'\n');
                                    output.append(static_cast<size_t>((level + 1) * indent), u' ');
                                }
                                SerializeJsonValueUtf16(array[i], output, indent, level + 1);
                            }
                            if (indent >= 0 && !array.empty()) {
                                output.push_back(u'\n');
                                output.append(static_cast<size_t>(level * indent), u' ');
                            }
                            output.push_back(u']');
                            return;
                        }
                        case 6: {
                            const auto& object = std::get<ConfigObject>(storage); // 保持字段插入顺序
                            output.push_back(u'{');
                            for (size_t i = 0; i < object.size(); ++i) {
                                if (i > 0) output.push_back(u',');
                                if (indent >= 0) {
                                    output.push_back(u'\n');
                                    output.append(static_cast<size_t>((level + 1) * indent), u' ');
                                }
                                AppendQuotedJsonString(output, object[i].key.View());
                                output.append(indent >= 0 ? u": " : u":");
                                SerializeJsonValueUtf16(object[i].value, output, indent, level + 1);
                            }
                            if (indent >= 0 && !object.empty()) {
                                output.push_back(u'\n');
                                output.append(static_cast<size_t>(level * indent), u' ');
                            }
                            output.push_back(u'}');
                            return;
                        }
                        default: output.append(u"null"); return;
                    }
                }
            }

            // 序列化完整文档时只构造一次最终 Core/String。
            String SerializeJsonDocument(const ConfigNode& value, int indent) {
                std::u16string output; // 最终 JSON5 UTF-16 缓冲
                if (indent < 0) {
                    // 紧凑路径：浅层预留 + 无美化分支写入。
                    output.reserve(GuessCompactJsonUtf16Capacity(value));
                    SerializeJsonValueUtf16Compact(value, output);
                } else {
                    output.reserve(4096);
                    SerializeJsonValueUtf16(value, output, indent, 0);
                }
                return String(std::u16string_view(output));
            }

            // YAML/TOML 内嵌 JSON 标量仍可向已有 String 缓冲追加完整片段。
            void SerializeJsonValue(const ConfigNode& value, String& output, int indent, int level) {
                std::u16string fragment; // 当前嵌入位置的 JSON 片段
                fragment.reserve(256);
                if (indent < 0) SerializeJsonValueUtf16Compact(value, fragment);
                else SerializeJsonValueUtf16(value, fragment, indent, level);
                output.Append(std::u16string_view(fragment));
            }
        }

        Result<ConfigValue> ConfigValue::TryParseJson(const String& text) {
            // Try* 接口保留错误状态，不抛出异常。
            return Internal::ParseJsonDocument(text);
        }

        ConfigValue ConfigValue::FromJson(const String& text) {
            auto result = TryParseJson(text); // 解析结果，失败时转换为异常接口
            if (!result.IsOk()) throw std::runtime_error(result.GetStatus().ToString().ToStdString());
            return result.MoveValue();
        }

        String ConfigValue::ToJson(int indent) const {
            return Internal::SerializeJsonDocument(ConfigValueAccess::Node(*this), indent);
        }

        // JSON5 显式入口复用兼容的 Json API，避免维护两套解析和序列化语义。
        Result<ConfigValue> ConfigValue::TryParseJson5(const String& text) {
            return TryParseJson(text);
        }

        ConfigValue ConfigValue::FromJson5(const String& text) {
            return FromJson(text);
        }

        String ConfigValue::ToJson5(int indent) const {
            return ToJson(indent);
        }
    }
}
