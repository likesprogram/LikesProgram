#include <LikesProgram/Config/ConfigurationInternal.hpp>

namespace LikesProgram {
    namespace Config {
        namespace Internal {
            namespace {
                // 递归下降 JSON 解析器；只依赖 Core/String，不引入第三方 JSON 库。
                class JsonParser {
                public:
                    // 解析前转成 UTF-32，后续位置计算按 code point 统一推进。
                    explicit JsonParser(const String& text)
                        : m_text(text.ToU32String()) {
                    }

                    // 解析完整文档，额外内容会作为错误返回。
                    Result<ConfigValue> Parse() {
                        try {
                            ConfigValue value = ParseValue(); // 根节点值，允许对象、数组和标量
                            SkipWhitespace();
                            if (m_pos != m_text.size()) return Error(u"unexpected data after JSON value", m_pos);
                            return value;
                        }
                        catch (const std::exception& ex) {
                            // 内部解析函数统一抛异常，入口统一转成 Status 诊断。
                            return Error(String(ex.what()), m_pos);
                        }
                    }

                private:
                    // 为错误补充当前位置行列，便于用户定位配置文件。
                    Result<ConfigValue> Error(const String& message, size_t position) const {
                        return Status::InvalidArgument(String(u"JSON parse error at ") +
                            BuildLineColumnMessage(m_text, position, message));
                    }

                    // 抛出解析错误；外层 Parse() 负责转成 Result。
                    void Fail(const String& message) const {
                        throw std::runtime_error(message.ToStdString());
                    }

                    // JSON 只承认 ASCII 空白，避免宽松 Unicode 空白改变语法。
                    void SkipWhitespace() {
                        while (m_pos < m_text.size() && IsAsciiSpace(m_text[m_pos])) ++m_pos;
                    }

                    // 预估当前容器的顶层元素数量，用于数组/对象预留容量。
                    size_t EstimateContainerItems(char32_t closing) const {
                        size_t pos = m_pos; // 从当前容器内容起点开始扫描
                        while (pos < m_text.size() && IsAsciiSpace(m_text[pos])) ++pos;
                        if (pos >= m_text.size() || m_text[pos] == closing) return 0;

                        size_t count = 1; // 非空容器至少包含一个顶层成员
                        size_t depth = 0; // 嵌套容器深度，0 表示当前容器顶层
                        bool inString = false; // 是否处于 JSON 字符串内部
                        bool escaped = false;  // 字符串中的反斜杠转义状态

                        for (; pos < m_text.size(); ++pos) {
                            char32_t ch = m_text[pos]; // 当前扫描字符
                            if (inString) {
                                if (escaped) escaped = false;
                                else if (ch == U'\\') escaped = true;
                                else if (ch == U'"') inString = false;
                                continue;
                            }

                            if (ch == U'"') inString = true;
                            else if (ch == U'{' || ch == U'[') ++depth;
                            else if (ch == U'}' || ch == U']') {
                                if (depth == 0 && ch == closing) break;
                                if (depth > 0) --depth;
                            }
                            else if (ch == U',' && depth == 0) {
                                ++count;
                            }
                        }

                        return count;
                    }

                    // 匹配 true/false/null 等固定字面量，成功后推进游标。
                    bool MatchLiteral(const char32_t* literal, size_t length) {
                        if (m_pos + length > m_text.size()) return false;
                        for (size_t i = 0; i < length; ++i) {
                            if (m_text[m_pos + i] != literal[i]) return false;
                        }
                        m_pos += length;
                        return true;
                    }

                    // 按首字符分派 JSON value，所有分支都消费完整值。
                    ConfigValue ParseValue() {
                        SkipWhitespace();
                        if (m_pos >= m_text.size()) Fail(u"unexpected end of input");

                        char32_t ch = m_text[m_pos]; // 当前 value 的首字符，用于选择解析分支
                        if (ch == U'{') return ParseObject();
                        if (ch == U'[') return ParseArray();
                        if (ch == U'"') return ConfigValue(ParseString());
                        // 固定字面量必须完整匹配，不能接受 truex 这类前缀误判。
                        if (ch == U't') {
                            if (!MatchLiteral(U"true", 4)) Fail(u"invalid literal");
                            return ConfigValue(true);
                        }
                        if (ch == U'f') {
                            if (!MatchLiteral(U"false", 5)) Fail(u"invalid literal");
                            return ConfigValue(false);
                        }
                        if (ch == U'n') {
                            if (!MatchLiteral(U"null", 4)) Fail(u"invalid literal");
                            return ConfigValue::Null();
                        }
                        if (ch == U'-' || IsAsciiDigit(ch)) return ParseNumber();

                        Fail(u"invalid JSON value");
                        return ConfigValue::Null();
                    }

                    // 解析对象，字段重复策略交给 ConfigValue::Set 覆盖旧值。
                    ConfigValue ParseObject() {
                        ++m_pos;
                        ConfigValue object = ConfigValue::Object(); // 当前对象节点，按字段出现顺序保存
                        ReserveObject(object, EstimateContainerItems(U'}'));

                        SkipWhitespace();
                        // 空对象是合法对象，直接消费右花括号。
                        if (m_pos < m_text.size() && m_text[m_pos] == U'}') {
                            ++m_pos;
                            return object;
                        }

                        while (true) {
                            SkipWhitespace();
                            if (m_pos >= m_text.size() || m_text[m_pos] != U'"') Fail(u"expected object key string");

                            String key = ParseString(); // 对象字段名，JSON 只允许字符串 key
                            SkipWhitespace();
                            if (m_pos >= m_text.size() || m_text[m_pos] != U':') Fail(u"expected ':' after object key");
                            ++m_pos;

                            // value 可递归为对象/数组；Set 会保持对象存储语义。
                            object.Set(key, ParseValue());
                            SkipWhitespace();

                            if (m_pos >= m_text.size()) Fail(u"unexpected end in object");
                            if (m_text[m_pos] == U'}') {
                                ++m_pos;
                                break;
                            }
                            if (m_text[m_pos] != U',') Fail(u"expected ',' between object fields");
                            ++m_pos;
                        }

                        return object;
                    }

                    // 解析数组，元素按出现顺序追加到 ConfigArray。
                    ConfigValue ParseArray() {
                        ++m_pos;
                        ConfigValue array = ConfigValue::Array(); // 当前数组节点，保留元素顺序
                        ReserveArray(array, EstimateContainerItems(U']'));

                        SkipWhitespace();
                        // 空数组直接消费右中括号。
                        if (m_pos < m_text.size() && m_text[m_pos] == U']') {
                            ++m_pos;
                            return array;
                        }

                        while (true) {
                            // 元素解析失败会抛出，外层统一带当前位置返回错误。
                            array.PushBack(ParseValue());
                            SkipWhitespace();

                            if (m_pos >= m_text.size()) Fail(u"unexpected end in array");
                            if (m_text[m_pos] == U']') {
                                ++m_pos;
                                break;
                            }
                            if (m_text[m_pos] != U',') Fail(u"expected ',' between array values");
                            ++m_pos;
                        }

                        return array;
                    }

                    // 解析 \uXXXX 的四位十六进制 payload。
                    char32_t ParseHex4() {
                        if (m_pos + 4 > m_text.size()) Fail(u"incomplete unicode escape");

                        char32_t value = 0; // 当前累计的 Unicode 转义数值
                        for (int i = 0; i < 4; ++i) {
                            char32_t ch = m_text[m_pos++]; // 当前十六进制字符
                            if (!IsHexDigit(ch)) Fail(u"invalid unicode escape digit");
                            value = static_cast<char32_t>((value << 4) + HexValue(ch));
                        }
                        return value;
                    }

                    // 解析 JSON 字符串，输出为已解码的 Unicode 文本。
                    String ParseString() {
                        if (m_text[m_pos] != U'"') Fail(u"expected string");
                        ++m_pos;

                        std::u32string output; // 已解码的 code point 输出缓冲
                        while (m_pos < m_text.size()) {
                            char32_t ch = m_text[m_pos++]; // 当前原始字符串字符
                            if (ch == U'"') return String(output);

                            if (ch < 0x20) Fail(u"control character in string");
                            if (ch != U'\\') {
                                output.push_back(ch);
                                continue;
                            }

                            if (m_pos >= m_text.size()) Fail(u"incomplete escape sequence");
                            char32_t escaped = m_text[m_pos++]; // 转义类型字符
                            switch (escaped) {
                            case U'"': output.push_back(U'"'); break;
                            case U'\\': output.push_back(U'\\'); break;
                            case U'/': output.push_back(U'/'); break;
                            case U'b': output.push_back(U'\b'); break;
                            case U'f': output.push_back(U'\f'); break;
                            case U'n': output.push_back(U'\n'); break;
                            case U'r': output.push_back(U'\r'); break;
                            case U't': output.push_back(U'\t'); break;
                            case U'u': {
                                char32_t high = ParseHex4(); // 高位或普通 Unicode 转义值
                                // JSON surrogate pair 必须成对出现，输出合并后的真实 code point。
                                if (high >= 0xD800 && high <= 0xDBFF) {
                                    if (m_pos + 2 > m_text.size() ||
                                        m_text[m_pos] != U'\\' || m_text[m_pos + 1] != U'u') {
                                        Fail(u"expected low surrogate after high surrogate");
                                    }
                                    m_pos += 2;
                                    char32_t low = ParseHex4(); // 低代理转义值，必须在 DC00-DFFF
                                    if (low < 0xDC00 || low > 0xDFFF) Fail(u"invalid low surrogate");
                                    output.push_back(0x10000 + ((high - 0xD800) << 10) + (low - 0xDC00));
                                }
                                else if (high >= 0xDC00 && high <= 0xDFFF) {
                                    // 单独低代理不是合法 Unicode scalar，直接拒绝。
                                    Fail(u"low surrogate without high surrogate");
                                }
                                else {
                                    output.push_back(high);
                                }
                                break;
                            }
                            default:
                                Fail(u"unsupported escape sequence");
                            }
                        }

                        Fail(u"unterminated string");
                        return String();
                    }

                    // 解析 JSON number，并按是否含小数/指数选择 int64 或 double。
                    ConfigValue ParseNumber() {
                        size_t begin = m_pos; // 数字 token 起始位置
                        bool isFloat = false; // true 表示需要按 double 严格解析

                        if (m_text[m_pos] == U'-') ++m_pos;
                        if (m_pos >= m_text.size()) Fail(u"incomplete number");

                        // 整数部分禁止前导零，保持 JSON 标准兼容。
                        if (m_text[m_pos] == U'0') {
                            ++m_pos;
                            if (m_pos < m_text.size() && IsAsciiDigit(m_text[m_pos])) Fail(u"leading zero is not allowed");
                        }
                        else if (IsAsciiDigit(m_text[m_pos])) {
                            while (m_pos < m_text.size() && IsAsciiDigit(m_text[m_pos])) ++m_pos;
                        }
                        else {
                            Fail(u"expected digit in number");
                        }

                        // 小数部分必须至少包含一位数字。
                        if (m_pos < m_text.size() && m_text[m_pos] == U'.') {
                            isFloat = true;
                            ++m_pos;
                            if (m_pos >= m_text.size() || !IsAsciiDigit(m_text[m_pos])) Fail(u"expected digit after decimal point");
                            while (m_pos < m_text.size() && IsAsciiDigit(m_text[m_pos])) ++m_pos;
                        }

                        // 指数部分允许正负号，但必须至少包含一位数字。
                        if (m_pos < m_text.size() && (m_text[m_pos] == U'e' || m_text[m_pos] == U'E')) {
                            isFloat = true;
                            ++m_pos;
                            if (m_pos < m_text.size() && (m_text[m_pos] == U'+' || m_text[m_pos] == U'-')) ++m_pos;
                            if (m_pos >= m_text.size() || !IsAsciiDigit(m_text[m_pos])) Fail(u"expected exponent digit");
                            while (m_pos < m_text.size() && IsAsciiDigit(m_text[m_pos])) ++m_pos;
                        }

                        String token(std::u32string_view(m_text.data() + begin, m_pos - begin)); // 完整数值 token
                        std::string text = token.ToStdString(); // from_chars 需要窄字节视图
                        if (isFloat) {
                            double value = 0.0; // 严格解析后的浮点值
                            if (!ParseDoubleStrict(token, value)) Fail(u"invalid floating number");
                            return ConfigValue(value);
                        }

                        int64_t value = 0; // 严格解析后的整数值
                        auto result = std::from_chars(text.data(), text.data() + text.size(), value); // 整数解析状态
                        if (result.ec != std::errc() || result.ptr != text.data() + text.size()) {
                            Fail(u"integer number is out of int64 range");
                        }
                        return ConfigValue(value);
                    }

                    std::u32string m_text; // 待解析文档，按 Unicode code point 索引
                    size_t m_pos = 0;      // 当前解析位置，单位为 code point
                };
            }

            // 包内 JSON 解析入口，隐藏具体递归下降实现。
            Result<ConfigValue> ParseJsonDocument(const String& text) {
                JsonParser parser(text); // 单次解析器，持有本次文档和游标
                return parser.Parse();
            }

            // 序列化任意 ConfigValue，indent < 0 时输出紧凑 JSON。
            void SerializeJsonValue(const ConfigValue& value, String& output, int indent, int level) {
                if (value.IsNull()) {
                    AppendUtf8(output, "null");
                    return;
                }
                if (value.IsString()) {
                    output.Append(QuoteJsonString(value.AsString()));
                    return;
                }
                if (value.IsInt64()) {
                    output.Append(String(value.AsInt64()));
                    return;
                }
                if (value.IsDouble()) {
                    output.Append(FormatDouble(value.AsDouble()));
                    return;
                }
                if (value.IsBool()) {
                    AppendUtf8(output, value.AsBool() ? "true" : "false");
                    return;
                }

                if (value.IsArray()) {
                    // 数组元素逐项递归输出；pretty 模式下每层增加 indent。
                    output.Append(u'[');
                    ForEachArrayItem(value, [&](size_t i, const ConfigValue& item) {
                        if (i > 0) output.Append(u',');
                        if (indent >= 0) {
                            output.Append(u'\n');
                            output.Append(RepeatSpaces(static_cast<size_t>((level + 1) * indent)));
                        }
                        SerializeJsonValue(item, output, indent, level + 1);
                    });
                    if (indent >= 0 && value.Size() > 0) {
                        output.Append(u'\n');
                        output.Append(RepeatSpaces(static_cast<size_t>(level * indent)));
                    }
                    output.Append(u']');
                    return;
                }

                if (value.IsObject()) {
                    // 对象按插入顺序输出字段，保证配置往返结果稳定。
                    output.Append(u'{');
                    size_t i = 0; // 当前输出字段序号，用于逗号和空对象判断
                    ForEachObjectEntry(value, [&](const String& key, const ConfigValue& child) {
                        if (i > 0) output.Append(u',');
                        if (indent >= 0) {
                            output.Append(u'\n');
                            output.Append(RepeatSpaces(static_cast<size_t>((level + 1) * indent)));
                        }
                        output.Append(QuoteJsonString(key));
                        AppendText(output, indent >= 0 ? u": " : u":");
                        SerializeJsonValue(child, output, indent, level + 1);
                        ++i;
                    });
                    if (indent >= 0 && i > 0) {
                        output.Append(u'\n');
                        output.Append(RepeatSpaces(static_cast<size_t>(level * indent)));
                    }
                    output.Append(u'}');
                }
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
            String output; // 序列化输出缓冲
            Internal::SerializeJsonValue(*this, output, indent, 0);
            return output;
        }
    }
}
