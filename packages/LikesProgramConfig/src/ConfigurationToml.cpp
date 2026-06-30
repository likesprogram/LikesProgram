#include <LikesProgram/Config/ConfigurationInternal.hpp>

namespace LikesProgram {
    namespace Config {
        namespace Internal {
            namespace {
                // 确保对象子节点存在，TOML table/dotted key 写入会复用它。
                ConfigValue& EnsureObjectChild(ConfigValue& node, const String& key);
                // 判断对象当前层是否已有 key，用于 inline table 去重。
                bool HasDirectKey(const ConfigValue& node, const String& key);

                // TOML 行内值解析器，负责字符串、数组、inline table 和基础标量。
                class TomlValueParser {
                public:
                    // 解析前转为 UTF-32，避免字符串/转义扫描受 UTF-16 surrogate 影响。
                    explicit TomlValueParser(const String& text)
                        : m_text(text.ToU32String()) {
                    }

                    // 解析完整值，尾随非空白内容视为错误。
                    Result<ConfigValue> Parse() {
                        try {
                            ConfigValue value = ParseValue(); // 当前行内值解析结果
                            SkipSpaces();
                            if (m_pos != m_text.size()) return Status::InvalidArgument(u"TOML parse error: unexpected data after value");
                            return value;
                        }
                        catch (const std::exception& ex) {
                            return Status::InvalidArgument(String(u"TOML parse error: ") + String(ex.what()));
                        }
                    }

                private:
                    // 抛出解析错误，由入口统一转换为 Status。
                    [[noreturn]] void Fail(const String& message) const {
                        throw std::runtime_error(message.ToStdString());
                    }

                    // TOML 行内结构只跳过空格和 tab，不跨越行边界。
                    void SkipSpaces() {
                        while (m_pos < m_text.size() && IsLineSpace(m_text[m_pos])) ++m_pos;
                    }

                    // 尝试消费指定分隔符，调用前会先跳过行内空白。
                    bool Consume(char32_t ch) {
                        SkipSpaces();
                        if (m_pos < m_text.size() && m_text[m_pos] == ch) {
                            ++m_pos;
                            return true;
                        }
                        return false;
                    }

                    // 预估当前 inline 容器的顶层成员数量，用于预留 vector 容量。
                    size_t EstimateInlineItems(char32_t closing) const {
                        size_t pos = m_pos; // 从容器内容起点开始扫描
                        while (pos < m_text.size() && IsLineSpace(m_text[pos])) ++pos;
                        if (pos >= m_text.size() || m_text[pos] == closing) return 0;

                        size_t count = 1; // 非空 inline 容器至少包含一个成员
                        size_t depth = 0; // 嵌套数组/table 深度
                        bool inSingle = false; // 是否处于单引号字符串
                        bool inDouble = false; // 是否处于双引号字符串
                        bool escaped = false;  // 双引号字符串内转义状态

                        for (; pos < m_text.size(); ++pos) {
                            char32_t ch = m_text[pos]; // 当前扫描字符
                            if (inDouble) {
                                if (escaped) escaped = false;
                                else if (ch == U'\\') escaped = true;
                                else if (ch == U'"') inDouble = false;
                                continue;
                            }
                            if (inSingle) {
                                if (ch == U'\'') inSingle = false;
                                continue;
                            }

                            if (ch == U'"') inDouble = true;
                            else if (ch == U'\'') inSingle = true;
                            else if (ch == U'[' || ch == U'{') ++depth;
                            else if (ch == U']' || ch == U'}') {
                                if (depth == 0 && ch == closing) break;
                                if (depth > 0) --depth;
                            }
                            else if (ch == U',' && depth == 0) {
                                ++count;
                            }
                        }

                        return count;
                    }

                    // 按首字符选择 TOML value 分支；裸 token 交给严格裸值解析。
                    ConfigValue ParseValue() {
                        SkipSpaces();
                        if (m_pos >= m_text.size()) Fail(u"empty value");

                        char32_t ch = m_text[m_pos]; // 当前 value 首字符
                        if (ch == U'"' || ch == U'\'') return ConfigValue(ParseString());
                        if (ch == U'[') return ParseArray();
                        if (ch == U'{') return ParseInlineTable();

                        size_t begin = m_pos; // 裸值 token 起始位置
                        while (m_pos < m_text.size() &&
                            m_text[m_pos] != U',' && m_text[m_pos] != U'}' && m_text[m_pos] != U']') {
                            ++m_pos;
                        }
                        String token = TrimLineSpace(String(std::u32string_view(m_text.data() + begin, m_pos - begin))); // 裸标量文本
                        return ParseBareValue(token);
                    }

                    // TOML 裸值只接受 bool/number，避免把拼写错误静默当字符串。
                    ConfigValue ParseBareValue(const String& token) const {
                        if (token == u"true") return ConfigValue(true);
                        if (token == u"false") return ConfigValue(false);

                        // 数字先按浮点特征分流，再回退到 int64 严格解析。
                        if (LooksLikeNumberToken(token)) {
                            if (token.Find(u".") != String::npos ||
                                token.Find(u"e") != String::npos ||
                                token.Find(u"E") != String::npos) {
                                double parsedDouble = 0.0; // TOML 浮点值，失败时说明裸 token 非法
                                if (ParseDoubleStrict(token, parsedDouble)) return ConfigValue(parsedDouble);
                            }

                            String intToken = token.StartsWith(u"+")
                                ? token.SubString(1, token.Size() - 1)
                                : token; // TOML 允许正号，from_chars 对正号兼容性不稳定
                            int64_t parsedInt = 0; // TOML 整数值，保持 int64 边界
                            if (ParseInt64Strict(intToken, parsedInt)) return ConfigValue(parsedInt);
                        }

                        Fail(u"invalid bare TOML value");
                    }

                    // 解析单引号/双引号字符串；双引号支持标准转义。
                    String ParseString() {
                        char32_t quote = m_text[m_pos++]; // 当前字符串引号类型
                        std::u32string output; // 已解码的字符串内容

                        while (m_pos < m_text.size()) {
                            char32_t ch = m_text[m_pos++]; // 当前字符串字符
                            if (ch == quote) return String(output);
                            if (quote == U'"' && ch == U'\\') {
                                output.push_back(ParseEscape());
                                continue;
                            }
                            output.push_back(ch);
                        }

                        Fail(u"unterminated string");
                    }

                    // 解析 TOML 双引号字符串中的转义序列。
                    char32_t ParseEscape() {
                        if (m_pos >= m_text.size()) Fail(u"incomplete escape");

                        char32_t escaped = m_text[m_pos++]; // 转义类型字符
                        switch (escaped) {
                        case U'"': return U'"';
                        case U'\\': return U'\\';
                        case U'b': return U'\b';
                        case U'f': return U'\f';
                        case U'n': return U'\n';
                        case U'r': return U'\r';
                        case U't': return U'\t';
                        case U'u': return ParseHexDigits(4);
                        case U'U': return ParseHexDigits(8);
                        default: Fail(u"invalid escape");
                        }
                    }

                    // 解析 \uXXXX 或 \UXXXXXXXX，并拒绝非法 Unicode scalar。
                    char32_t ParseHexDigits(size_t digits) {
                        if (m_pos + digits > m_text.size()) Fail(u"incomplete unicode escape");

                        char32_t value = 0; // 当前累计的 Unicode scalar
                        for (size_t i = 0; i < digits; ++i) {
                            char32_t ch = m_text[m_pos++]; // 当前十六进制字符
                            if (!IsHexDigit(ch)) Fail(u"invalid unicode escape digit");
                            value = static_cast<char32_t>((value << 4) + HexValue(ch));
                        }
                        if (value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) {
                            Fail(u"invalid unicode scalar value");
                        }
                        return value;
                    }

                    // 解析 TOML 数组，允许尾随逗号。
                    ConfigValue ParseArray() {
                        ++m_pos;
                        ConfigValue array = ConfigValue::Array(); // 当前数组节点
                        ReserveArray(array, EstimateInlineItems(U']'));
                        SkipSpaces();
                        if (Consume(U']')) return array;

                        while (true) {
                            // 元素可以是标量、数组或 inline table。
                            array.PushBack(ParseValue());
                            SkipSpaces();
                            if (Consume(U']')) break;
                            if (!Consume(U',')) Fail(u"expected ',' in array");
                            SkipSpaces();
                            if (Consume(U']')) break;
                        }
                        return array;
                    }

                    // 解析 inline table，并支持内部 dotted key。
                    ConfigValue ParseInlineTable() {
                        ++m_pos;
                        ConfigValue object = ConfigValue::Object(); // 当前 inline table 对象
                        ReserveObject(object, EstimateInlineItems(U'}'));
                        SkipSpaces();
                        if (Consume(U'}')) return object;

                        while (true) {
                            auto path = ParseDottedKey(); // 当前字段的 dotted path
                            if (!Consume(U'=')) Fail(u"expected '=' in inline table");
                            if (path.empty()) Fail(u"empty key");

                            ConfigValue* current = &object; // dotted key 写入当前位置，避免丢失层级语义
                            for (size_t i = 0; i + 1 < path.size(); ++i) {
                                current = &EnsureObjectChild(*current, path[i]);
                            }
                            if (HasDirectKey(*current, path.back())) Fail(u"duplicate key in inline table");
                            current->Set(path.back(), ParseValue());
                            SkipSpaces();
                            if (Consume(U'}')) break;
                            if (!Consume(U',')) Fail(u"expected ',' in inline table");
                        }
                        return object;
                    }

                    // 解析 inline table 内部的 dotted key。
                    std::vector<String> ParseDottedKey() {
                        std::vector<String> parts; // inline table key 也支持 TOML dotted key
                        while (true) {
                            parts.push_back(ParseKeySegment());
                            SkipSpaces();
                            if (m_pos >= m_text.size() || m_text[m_pos] != U'.') break;
                            ++m_pos;
                        }
                        return parts;
                    }

                    // 解析裸 key 或 quoted key 的单个 path 片段。
                    String ParseKeySegment() {
                        SkipSpaces();
                        if (m_pos >= m_text.size()) Fail(u"expected key");
                        if (m_text[m_pos] == U'"' || m_text[m_pos] == U'\'') return ParseString();

                        size_t begin = m_pos; // 裸 key 起始位置
                        while (m_pos < m_text.size() && !IsLineSpace(m_text[m_pos]) &&
                            m_text[m_pos] != U'.' && m_text[m_pos] != U'=' &&
                            m_text[m_pos] != U',' && m_text[m_pos] != U'}') {
                            ++m_pos;
                        }
                        String key = TrimLineSpace(String(std::u32string_view(m_text.data() + begin, m_pos - begin))); // 当前裸 key 文本
                        if (key.Empty()) Fail(u"empty key");
                        return key;
                    }

                    std::u32string m_text; // 待解析的 TOML 行内值，按 code point 索引
                    size_t m_pos = 0;      // 当前解析位置，单位为 code point
                };

                // 解析单个 TOML value 文本。
                Result<ConfigValue> ParseTomlValue(const String& text) {
                    TomlValueParser parser(text); // 单次解析器，持有行内 value 游标
                    return parser.Parse();
                }

                // 拆分 TOML dotted key，quoted 片段允许包含点号。
                Result<std::vector<String>> SplitTomlDottedPath(const String& key, size_t line) {
                    std::u32string text = key.ToU32String(); // 原始 key 文本，允许带引号的 dotted key 片段
                    std::vector<String> parts; // 解析后的路径片段，供 SetPath 逐级写入
                    size_t pos = 0; // 当前 code point 偏移

                    auto fail = [line](const String& message) -> Result<std::vector<String>> {
                        return Status::InvalidArgument(String::Format(
                            u"TOML parse error at line {}: {}", line, message));
                    };

                    while (pos < text.size()) {
                        while (pos < text.size() && IsLineSpace(text[pos])) ++pos;
                        if (pos >= text.size()) break;

                        String part;
                        if (text[pos] == U'"' || text[pos] == U'\'') {
                            char32_t quote = text[pos++]; // 当前 key 片段的引号类型
                            std::u32string decoded; // 去掉引号后的 key 片段文本
                            while (pos < text.size() && text[pos] != quote) {
                                if (quote == U'"' && text[pos] == U'\\') {
                                    size_t escapeStart = pos++; // 转义序列起始位置，用于错误诊断
                                    if (pos >= text.size()) return fail(u"incomplete quoted key escape");
                                    char32_t escaped = text[pos++];
                                    switch (escaped) {
                                    case U'"': decoded.push_back(U'"'); break;
                                    case U'\\': decoded.push_back(U'\\'); break;
                                    case U'b': decoded.push_back(U'\b'); break;
                                    case U'f': decoded.push_back(U'\f'); break;
                                    case U'n': decoded.push_back(U'\n'); break;
                                    case U'r': decoded.push_back(U'\r'); break;
                                    case U't': decoded.push_back(U'\t'); break;
                                    default:
                                        (void)escapeStart;
                                        return fail(u"invalid quoted key escape");
                                    }
                                    continue;
                                }
                                decoded.push_back(text[pos++]);
                            }
                            if (pos >= text.size() || text[pos] != quote) return fail(u"unterminated quoted key");
                            ++pos;
                            part = String(decoded);
                        }
                        else {
                            size_t begin = pos; // 当前裸 key 片段起始位置
                            while (pos < text.size() && text[pos] != U'.') ++pos;
                            part = TrimLineSpace(String(std::u32string_view(text.data() + begin, pos - begin)));
                        }

                        if (part.Empty()) return fail(u"empty key segment");
                        parts.push_back(part);

                        while (pos < text.size() && IsLineSpace(text[pos])) ++pos;
                        if (pos >= text.size()) break;
                        if (text[pos] != U'.') return fail(u"unexpected character in dotted key");
                        ++pos;
                    }

                    if (parts.empty()) return fail(u"empty key");
                    return parts;
                }

                // 将任意节点提升为对象节点，供 table/dotted key 写入。
                void EnsureObjectNode(ConfigValue& node) {
                    if (node.IsObject()) return;

                    // 直接替换内部存储，避免通过 Set 临时字段绕一圈。
                    ConfigValueAccess::Storage(node) = ConfigObject{};
                }

                // 返回指定 key 的对象子节点，缺失时创建对象节点。
                ConfigValue& EnsureObjectChild(ConfigValue& node, const String& key) {
                    EnsureObjectNode(node);
                    auto* object = ConfigValueAccess::Object(node); // 当前节点已保证为对象

                    // 已存在字段时复用对象存储，标量/table 冲突按 TOML 规则报错。
                    if (auto* entry = FindObjectEntry(*object, key)) {
                        if (!entry->value.IsObject()) {
                            throw std::runtime_error("TOML table path conflicts with scalar value");
                        }
                        return entry->value;
                    }

                    object->push_back(ConfigObjectEntry{ key, ConfigValue::Object() });
                    return object->back().value;
                }

                // 检查当前对象层是否存在直接字段，不解释 dotted path。
                bool HasDirectKey(const ConfigValue& node, const String& key) {
                    const auto* object = ConfigValueAccess::Object(node); // 直接字段检查，不把 quoted dotted key 当路径
                    return object && FindObjectEntry(*object, key);
                }

                // 确保普通 [table] path 存在并返回末端对象。
                ConfigValue& EnsureTablePath(ConfigValue& root, const std::vector<String>& path) {
                    if (path.empty()) return root;

                    ConfigValue* current = &root; // 当前 table path 节点
                    for (const auto& part : path) {
                        current = &EnsureObjectChild(*current, part);
                    }
                    return *current;
                }

                // 按 dotted path 写入值，重复 key 按 TOML 规则报错。
                void SetPath(ConfigValue& root, const std::vector<String>& path, ConfigValue value) {
                    if (path.empty()) return;

                    // dotted path 写入采用原地逐级下降，避免每行 TOML 都复制根对象。
                    ConfigValue* current = &root;
                    for (size_t i = 0; i + 1 < path.size(); ++i) {
                        current = &EnsureObjectChild(*current, path[i]);
                    }

                    if (HasDirectKey(*current, path.back())) {
                        throw std::runtime_error("duplicate TOML key");
                    }
                    current->Set(path.back(), std::move(value));
                }

                // 处理 [[table]]，每次出现都追加一个新的对象元素。
                ConfigValue& EnsureArrayTable(ConfigValue& root, const std::vector<String>& path) {
                    if (path.empty()) return root;

                    ConfigValue* parent = &root; // 指向 table array 所属父对象
                    for (size_t i = 0; i + 1 < path.size(); ++i) {
                        parent = &EnsureObjectChild(*parent, path[i]);
                    }

                    EnsureObjectNode(*parent);
                    auto* object = ConfigValueAccess::Object(*parent); // 父节点已保证为对象
                    auto* entry = FindObjectEntry(*object, path.back());
                    if (!entry) {
                        object->push_back(ConfigObjectEntry{ path.back(), ConfigValue::Array() });
                        entry = &object->back();
                    }
                    if (!entry->value.IsArray()) {
                        throw std::runtime_error("array table conflicts with existing non-array value");
                    }

                    entry->value.PushBack(ConfigValue::Object());
                    auto* array = ConfigValueAccess::Array(entry->value); // 已保证为数组
                    return array->back();
                }

                // 查找 TOML 赋值等号，忽略 quoted key 中的等号。
                bool FindTomlAssignment(const String& line, size_t& index) {
                    std::u32string text = line.ToU32String(); // key=value 行，等号可能出现在 quoted key 内
                    bool inSingle = false; // 当前是否位于单引号 key
                    bool inDouble = false; // 当前是否位于双引号 key
                    bool escaped = false;  // 双引号 key 内的转义状态

                    for (size_t i = 0; i < text.size(); ++i) {
                        char32_t ch = text[i]; // 当前 code point
                        if (inDouble) {
                            if (escaped) escaped = false;
                            else if (ch == U'\\') escaped = true;
                            else if (ch == U'"') inDouble = false;
                            continue;
                        }

                        if (inSingle) {
                            if (ch == U'\'') inSingle = false;
                            continue;
                        }

                        if (ch == U'"') inDouble = true;
                        else if (ch == U'\'') inSingle = true;
                        else if (ch == U'=') {
                            index = i;
                            return true;
                        }
                    }

                    return false;
                }

                // 判断 key 是否可以直接按裸 key 输出。
                bool IsBareTomlKey(const String& key) {
                    if (key.Empty()) return false;
                    for (char32_t ch : key) {
                        if ((ch >= U'a' && ch <= U'z') ||
                            (ch >= U'A' && ch <= U'Z') ||
                            (ch >= U'0' && ch <= U'9') ||
                            ch == U'_' || ch == U'-') {
                            continue;
                        }
                        return false;
                    }
                    return true;
                }

                // 选择裸 key 或 JSON 风格 quoted key，保证 TOML 输出可再解析。
                String FormatTomlKey(const String& key) {
                    return IsBareTomlKey(key) ? key : QuoteJsonString(key);
                }

                String FormatTomlInlineValue(const ConfigValue& value);

                // 将数组序列化为 TOML inline array。
                String FormatTomlArray(const ConfigValue& value) {
                    String output; // inline array 输出缓冲
                    output.Append(u'[');
                    ForEachArrayItem(value, [&](size_t index, const ConfigValue& item) {
                        if (index > 0) AppendText(output, u", ");
                        output.Append(FormatTomlInlineValue(item));
                    });
                    output.Append(u']');
                    return output;
                }

                // 将对象序列化为 TOML inline table。
                String FormatTomlInlineTable(const ConfigValue& value) {
                    String output; // inline table 输出缓冲
                    output.Append(u'{');
                    bool first = true; // 控制 inline table 字段分隔符
                    ForEachObjectEntry(value, [&](const String& key, const ConfigValue& child) {
                        if (!first) AppendText(output, u", ");
                        first = false;
                        output.Append(FormatTomlKey(key));
                        AppendText(output, u" = ");
                        output.Append(FormatTomlInlineValue(child));
                    });
                    output.Append(u'}');
                    return output;
                }

                // 标量和复合值的 inline TOML 表达。
                String FormatTomlInlineValue(const ConfigValue& value) {
                    if (value.IsNull()) return QuoteJsonString(String());
                    if (value.IsString()) return QuoteJsonString(value.AsString());
                    if (value.IsInt64()) return String(value.AsInt64());
                    if (value.IsDouble()) return FormatDouble(value.AsDouble());
                    if (value.IsBool()) return value.AsBool() ? String(u"true") : String(u"false");
                    if (value.IsArray()) return FormatTomlArray(value);
                    if (value.IsObject()) return FormatTomlInlineTable(value);
                    return QuoteJsonString(String());
                }

                // 输出一行 key = value 赋值。
                void AppendTomlAssignment(String& output, const String& key, const ConfigValue& value) {
                    output.Append(FormatTomlKey(key));
                    AppendText(output, u" = ");
                    output.Append(FormatTomlInlineValue(value));
                    output.Append(u'\n');
                }

                // 先输出当前 table 标量字段，再递归输出子 table。
                void SerializeTomlTable(const ConfigValue& table, const String& path, String& output) {
                    ForEachObjectEntry(table, [&](const String& key, const ConfigValue& child) {
                        if (child.IsObject()) return;
                        AppendTomlAssignment(output, key, child);
                    });

                    ForEachObjectEntry(table, [&](const String& key, const ConfigValue& child) {
                        if (!child.IsObject()) return;

                        String childPath = path.Empty() ? FormatTomlKey(key) : path + u"." + FormatTomlKey(key);
                        output.Append(u'\n');
                        output.Append(u'[');
                        output.Append(childPath);
                        AppendText(output, u"]\n");
                        SerializeTomlTable(child, childPath, output);
                    });
                }
            }

            // 解析完整 TOML 文档，根节点固定为对象。
            Result<ConfigValue> ParseTomlDocument(const String& text) {
                ConfigValue root = ConfigValue::Object(); // TOML 文档根对象
                std::vector<String> currentPath; // 当前 [table] 的路径
                ConfigValue* currentArrayTable = nullptr; // 非空时表示后续赋值写入最近的 [[table]]
                auto lines = SplitLines(text); // 原始行列表，错误诊断使用索引转行号
                ReserveObject(root, lines.size());

                try {
                    for (size_t i = 0; i < lines.size(); ++i) {
                        String line = StripLineComment(lines[i], U'#'); // 当前有效 TOML 行
                        if (line.Empty()) continue;

                        if (line.StartsWith(u"[") && line.EndsWith(u"]")) {
                            bool arrayTable = line.StartsWith(u"[[") && line.EndsWith(u"]]"); // 是否为 table array
                            size_t drop = arrayTable ? 2 : 1; // 需要去掉的左右括号层数
                            String section = TrimLineSpace(line.SubString(drop, line.Size() - drop * 2)); // table 名文本
                            if (section.Empty()) {
                                return Status::InvalidArgument(String::Format(
                                    u"TOML parse error at line {}: empty table name", i + 1));
                            }
                            auto parsedSection = SplitTomlDottedPath(section, i + 1); // table 路径片段
                            if (!parsedSection.IsOk()) return parsedSection.GetStatus();
                            currentPath = parsedSection.Value();
                            if (arrayTable) {
                                currentArrayTable = &EnsureArrayTable(root, currentPath);
                            }
                            else {
                                EnsureTablePath(root, currentPath);
                                currentArrayTable = nullptr;
                            }
                            continue;
                        }

                        size_t equals = 0; // 当前赋值行的顶层等号位置
                        if (!FindTomlAssignment(line, equals)) {
                            return Status::InvalidArgument(String::Format(
                                u"TOML parse error at line {}: expected key = value", i + 1));
                        }

                        String key = TrimLineSpace(line.SubString(0, equals)); // 左侧 key 文本
                        String valueText = TrimLineSpace(line.SubString(equals + 1, line.Size() - equals - 1)); // 右侧 value 文本
                        if (key.Empty()) {
                            return Status::InvalidArgument(String::Format(
                                u"TOML parse error at line {}: empty key", i + 1));
                        }

                        auto parsed = ParseTomlValue(valueText); // 解析后的右值
                        if (!parsed.IsOk()) return parsed.GetStatus();

                        auto parsedKey = SplitTomlDottedPath(key, i + 1); // 赋值 key 路径片段
                        if (!parsedKey.IsOk()) return parsedKey.GetStatus();
                        auto keyParts = parsedKey.Value(); // 当前赋值相对路径
                        if (currentArrayTable) {
                            SetPath(*currentArrayTable, keyParts, parsed.MoveValue());
                        }
                        else {
                            std::vector<String> fullPath = currentPath; // table 路径 + key 路径
                            fullPath.insert(fullPath.end(), keyParts.begin(), keyParts.end());
                            SetPath(root, fullPath, parsed.MoveValue());
                        }
                    }
                }
                catch (const std::exception& ex) {
                    return Status::InvalidArgument(String(u"TOML parse error: ") + String(ex.what()));
                }

                return root;
            }

            // 将对象根序列化为 TOML；非对象根不输出文档。
            String SerializeTomlDocument(const ConfigValue& value) {
                if (!value.IsObject()) return String();

                String output; // TOML 文档输出缓冲
                SerializeTomlTable(value, String(), output);
                return output;
            }
        }

        Result<ConfigValue> ConfigValue::TryParseToml(const String& text) {
            // Try* 接口保留错误状态，不抛出异常。
            return Internal::ParseTomlDocument(text);
        }

        ConfigValue ConfigValue::FromToml(const String& text) {
            auto result = TryParseToml(text); // 解析结果，失败时转成异常接口
            if (!result.IsOk()) throw std::runtime_error(result.GetStatus().ToString().ToStdString());
            return result.MoveValue();
        }

        String ConfigValue::ToToml() const {
            // TOML 只能表达对象根，具体判定由内部序列化入口负责。
            return Internal::SerializeTomlDocument(*this);
        }
    }
}
