#include "LikesProgram/Config/ConfigurationInternal.hpp"
#include <cstring>
#include <new>
#include <utility>

namespace LikesProgram {
    namespace Config {
        namespace Internal {
            ConfigText::ConfigText(const String& value) {
                new (&m_storage.stringValue) String(value);
                SetState(OriginalStringState);
            }

            ConfigText::ConfigText(String&& value) {
                new (&m_storage.stringValue) String(std::move(value));
                SetState(OriginalStringState);
            }

            ConfigText::ConfigText(std::u16string_view value) {
                Assign(value);
            }

            ConfigText::ConfigText(std::u16string&& value) {
                Assign(std::u16string_view(value));
            }

            ConfigText::ConfigText(const ConfigText& other) {
                if (other.State() == OriginalStringState) {
                    new (&m_storage.stringValue) String(other.m_storage.stringValue);
                    SetState(OriginalStringState);
                } else if (other.State() == MaterializedStringState) {
                    new (&m_storage.stringValue) String(*other.m_storage.materializedValue);
                    SetState(OriginalStringState);
                } else Assign(other.View());
            }

            ConfigText::ConfigText(ConfigText&& other) {
                const unsigned char state = other.State();
                if (state == OriginalStringState) {
                    new (&m_storage.stringValue) String(std::move(other.m_storage.stringValue));
                    SetState(OriginalStringState);
                    // Keep the moved-from String state so its normal destructor releases it once.
                    return;
                } else if (state == MaterializedStringState) {
                    m_storage.materializedValue = other.m_storage.materializedValue;
                    SetState(MaterializedStringState);
                } else if (state == HeapState) {
                    m_storage.heapText = other.m_storage.heapText;
                    SetState(HeapState);
                } else {
                    if (state > 0) std::memcpy(m_storage.inlineData, other.m_storage.inlineData, static_cast<size_t>(state) * sizeof(char16_t));
                    SetState(state);
                }
                other.SetState(0);
            }

            ConfigText::~ConfigText() {
                Release();
            }

            ConfigText& ConfigText::operator=(const ConfigText& other) {
                if (this == &other) return *this;
                ConfigText replacement(other); // 先完成可能抛出的深拷贝
                return *this = std::move(replacement);
            }

            ConfigText& ConfigText::operator=(ConfigText&& other) {
                if (this == &other) return *this;
                this->~ConfigText();
                new (this) ConfigText(std::move(other));
                return *this;
            }

            void ConfigText::Assign(std::u16string_view value) {
                if (value.size() <= InlineCapacity) {
                    if (!value.empty()) std::memcpy(m_storage.inlineData, value.data(), value.size() * sizeof(char16_t));
                    SetState(static_cast<unsigned char>(value.size()));
                    return;
                }

                std::unique_ptr<char16_t[]> data = std::make_unique<char16_t[]>(value.size()); // 长文本独立缓冲
                std::memcpy(data.get(), value.data(), value.size() * sizeof(char16_t));
                m_storage.heapText.data = data.release();
                m_storage.heapText.length = value.size();
                SetState(HeapState);
            }

            const char16_t* ConfigText::Data() const noexcept {
                static constexpr char16_t empty[] = u""; // 空 view 的稳定地址
                const unsigned char state = State();
                if (state == OriginalStringState) return m_storage.stringValue.data();
                if (state == MaterializedStringState) return m_storage.materializedValue->data();
                if (state == HeapState) return m_storage.heapText.data;
                if (state == 0) return empty;
                return m_storage.inlineData;
            }

            unsigned char ConfigText::State() const noexcept {
                const auto* bytes = reinterpret_cast<const unsigned char*>(&m_storage);
                return bytes[StateOffset];
            }

            void ConfigText::SetState(unsigned char state) const noexcept {
                auto* bytes = reinterpret_cast<unsigned char*>(&m_storage);
                bytes[StateOffset] = state;
            }

            void ConfigText::Release() noexcept {
                const unsigned char state = State();
                if (state == OriginalStringState) m_storage.stringValue.~String();
                else if (state == MaterializedStringState) delete m_storage.materializedValue;
                else if (state == HeapState) delete[] m_storage.heapText.data;
            }

            std::u16string_view ConfigText::View() const noexcept {
                const unsigned char state = State();
                if (state == OriginalStringState) return std::u16string_view(m_storage.stringValue.data(), m_storage.stringValue.Length());
                if (state == MaterializedStringState) return std::u16string_view(m_storage.materializedValue->data(), m_storage.materializedValue->Length());
                if (state == HeapState) return std::u16string_view(m_storage.heapText.data, m_storage.heapText.length);
                return std::u16string_view(Data(), state);
            }

            const String& ConfigText::AsString() const {
                const unsigned char state = State();
                if (state == OriginalStringState) return m_storage.stringValue;
                if (state == MaterializedStringState) return *m_storage.materializedValue;

                std::unique_ptr<String> converted = std::make_unique<String>(View()); // 成功后再替换延迟存储
                if (state == HeapState) delete[] m_storage.heapText.data;
                m_storage.materializedValue = converted.release();
                SetState(MaterializedStringState);
                return *m_storage.materializedValue;
            }

            bool ConfigText::Equals(const String& value) const noexcept {
                const auto current = View(); // 当前 key 的 UTF-16 view
                const auto other = std::u16string_view(value.data(), value.Length()); // 查询 key view
                return current == other;
            }

            bool ConfigText::operator==(const ConfigText& other) const noexcept {
                return View() == other.View();
            }

            // 默认节点持有 null，供公开根恢复和共享只读哨兵使用。
            ConfigNode::ConfigNode() = default;

            // 字符串直接在节点 variant 中就位，避免临时 storage 再搬移一次。
            ConfigNode::ConfigNode(ConfigText&& value) : m_value(std::move(value)) { }

            // 标量直接选择唯一 variant 分支。
            ConfigNode::ConfigNode(int64_t value) noexcept : m_value(value) { }

            ConfigNode::ConfigNode(double value) noexcept : m_value(value) { }

            ConfigNode::ConfigNode(bool value) noexcept : m_value(value) { }

            // 容器直接把已完成的递归字段表或元素表移入节点。
            ConfigNode::ConfigNode(ConfigArray&& value) : m_value(std::move(value)) { }

            ConfigNode::ConfigNode(ConfigObject&& value) : m_value(std::move(value)) { }

            // 节点只允许移动，throwing move 由 vector 的基本保证承接。
            ConfigNode::ConfigNode(ConfigNode&& other) = default;

            // 递归容器按正常逆序销毁整棵内部树。
            ConfigNode::~ConfigNode() = default;

            // 移动赋值替换完整子树，不允许隐式深复制。
            ConfigNode& ConfigNode::operator=(ConfigNode&& other) = default;

            // variant 内容比较递归复用数组和对象条目的顺序比较。
            bool ConfigNode::operator==(const ConfigNode& other) const {
                return m_value == other.m_value;
            }

            // 显式深克隆内部树，复制失败时局部 RAII 容器负责完整回收。
            ConfigNode CloneNode(const ConfigNode& source) {
                const auto& storage = source.m_value; // 当前只读节点分支
                if (const auto* text = std::get_if<ConfigText>(&storage)) return ConfigNode(ConfigText(*text));
                if (const auto* integer = std::get_if<int64_t>(&storage)) return ConfigNode(*integer);
                if (const auto* number = std::get_if<double>(&storage)) return ConfigNode(*number);
                if (const auto* boolean = std::get_if<bool>(&storage)) return ConfigNode(*boolean);

                if (const auto* array = std::get_if<ConfigArray>(&storage)) {
                    ConfigArray clone; // 目标数组按值拥有全部深克隆子节点
                    clone.reserve(array->size());
                    for (const auto& child : *array) clone.push_back(CloneNode(child));
                    return ConfigNode(std::move(clone));
                }

                if (const auto* object = std::get_if<ConfigObject>(&storage)) {
                    ConfigObject clone; // 目标对象保持 key 和字段插入顺序
                    clone.reserve(object->size());
                    for (const auto& entry : *object) clone.push_back(ConfigObjectEntry{ ConfigText(entry.key), CloneNode(entry.value) });
                    return ConfigNode(std::move(clone));
                }

                return ConfigNode();
            }

            // variant 分支索引和公开枚举保持一一对应，未知分支安全回退 null。
            ConfigValueType NodeType(const ConfigNode& value) noexcept {
                switch (value.m_value.index()) {
                    case 0: return ConfigValueType::Null;
                    case 1: return ConfigValueType::String;
                    case 2: return ConfigValueType::Int64;
                    case 3: return ConfigValueType::Double;
                    case 4: return ConfigValueType::Bool;
                    case 5: return ConfigValueType::Array;
                    case 6: return ConfigValueType::Object;
                    default: return ConfigValueType::Null;
                }
            }

            // 内部遍历只比较枚举，不分配公开值或触发深克隆。
            bool NodeIs(const ConfigNode& value, ConfigValueType type) noexcept {
                return NodeType(value) == type;
            }

            // 字符串节点按需物化 Core/String，其他类型共享空串哨兵。
            const String& NodeString(const ConfigNode& value) {
                static const String empty; // 非字符串内部节点共享的稳定返回值
                const auto* text = std::get_if<ConfigText>(&value.m_value); // 私有文本分支
                return text ? text->AsString() : empty;
            }

            // JSON/YAML/TOML 共同使用的 ASCII 空白判断。
            bool IsAsciiSpace(char32_t ch) noexcept {
                return ch == U' ' || ch == U'\t' || ch == U'\r' || ch == U'\n';
            }

            // 行内空白只包含空格和 tab，避免跨行吞掉结构。
            bool IsLineSpace(char32_t ch) noexcept {
                return ch == U' ' || ch == U'\t';
            }

            // 配置语法数字统一只接受 ASCII 数字。
            bool IsAsciiDigit(char32_t ch) noexcept {
                return ch >= U'0' && ch <= U'9';
            }

            // Unicode 转义和十六进制输出共用的数字判断。
            bool IsHexDigit(char32_t ch) noexcept {
                return (ch >= U'0' && ch <= U'9') || (ch >= U'a' && ch <= U'f') || (ch >= U'A' && ch <= U'F');
            }

            // 将十六进制字符转换为数值，调用方负责先判断合法性。
            int HexValue(char32_t ch) {
                if (ch >= U'0' && ch <= U'9') return static_cast<int>(ch - U'0');
                if (ch >= U'a' && ch <= U'f') return static_cast<int>(ch - U'a' + 10);
                if (ch >= U'A' && ch <= U'F') return static_cast<int>(ch - U'A' + 10);
                return -1;
            }

            // 去除首尾 ASCII 空白，配置 key/value 归一化复用。
            String TrimAscii(const String& value) {
                std::u32string text = value.ToU32String(); // 待裁剪文本
                size_t begin = 0; // 首个非空白 code point 位置
                size_t end = text.size(); // 尾后位置

                while (begin < end && IsAsciiSpace(text[begin])) ++begin;
                while (end > begin && IsAsciiSpace(text[end - 1])) --end;

                return String(std::u32string_view(text.data() + begin, end - begin));
            }

            // 只裁剪行内空白，YAML/TOML 分隔结构使用。
            String TrimLineSpace(const String& value) {
                std::u32string text = value.ToU32String(); // 待裁剪文本
                size_t begin = 0; // 首个非行内空白位置
                size_t end = text.size(); // 尾后位置

                while (begin < end && IsLineSpace(text[begin])) ++begin;
                while (end > begin && IsLineSpace(text[end - 1])) --end;

                return String(std::u32string_view(text.data() + begin, end - begin));
            }

            // 按 LF/CRLF 拆行，保留最后一行即使为空。
            std::vector<String> SplitLines(const String& text) {
                std::vector<String> lines; // 输出行列表
                std::u32string data = text.ToU32String(); // 输入文档 code point 缓冲
                size_t begin = 0; // 当前行起始位置

                for (size_t i = 0; i < data.size(); ++i) {
                    if (data[i] != U'\n') continue;

                    size_t count = i - begin; // 当前行长度，不含 LF
                    if (count > 0 && data[begin + count - 1] == U'\r') --count;
                    lines.emplace_back(std::u32string_view(data.data() + begin, count));
                    begin = i + 1;
                }

                if (begin <= data.size()) lines.emplace_back(std::u32string_view(data.data() + begin, data.size() - begin));

                return lines;
            }

            // 查找 key=value 中的等号，供简单配置和 TOML 辅助使用。
            size_t FindEquals(const String& line) {
                size_t index = 0; // 当前 code point 位置
                for (auto cp : line) {
                    if (cp == U'=') return index;
                    ++index;
                }
                return String::npos;
            }

            // 判断 key 是否含 dotted path 分隔符。
            bool HasDot(const String& key) {
                for (auto cp : key) if (cp == U'.') return true;
                return false;
            }

            // 将 dotted path 拆为非空片段，连续点会被忽略为空片段。
            std::vector<String> SplitDottedPath(const String& key) {
                std::vector<String> parts; // 输出路径片段
                std::u32string data = key.ToU32String(); // key 的 code point 文本
                size_t begin = 0; // 当前片段起始位置

                for (size_t i = 0; i <= data.size(); ++i) {
                    if (i < data.size() && data[i] != U'.') continue;

                    if (i > begin) parts.emplace_back(std::u32string_view(data.data() + begin, i - begin));
                    begin = i + 1;
                }

                return parts;
            }

            // 严格 int64 解析，失败不修改输出语义由调用方决定。
            bool ParseInt64Strict(const String& value, int64_t& out) {
                std::string text = TrimAscii(value).ToStdString(); // from_chars 输入文本
                if (text.empty()) return false;

                int64_t parsed = 0; // 解析后的整数值
                const char* begin = text.data(); // 输入首地址
                const char* end = text.data() + text.size(); // 输入尾后地址
                auto result = std::from_chars(begin, end, parsed); // 解析状态
                if (result.ec != std::errc() || result.ptr != end) return false;

                out = parsed;
                return true;
            }

            // 严格 double 解析，拒绝尾随垃圾和非有限值。
            bool ParseDoubleStrict(const String& value, double& out) {
                std::string text = TrimAscii(value).ToStdString(); // strtod 输入文本
                if (text.empty()) return false;

                char* parsedEnd = nullptr; // strtod 写入的尾后指针
                double parsed = std::strtod(text.c_str(), &parsedEnd); // 解析后的浮点值
                if (parsedEnd == text.c_str() || *parsedEnd != '\0' || !std::isfinite(parsed)) return false;

                out = parsed;
                return true;
            }

            // 严格 bool 解析，支持配置常见同义词。
            bool ParseBoolStrict(const String& value, bool& out) {
                String normalized = TrimAscii(value).ToLower(); // 小写归一化文本
                if (normalized == u"true" || normalized == u"1" || normalized == u"yes" || normalized == u"on") {
                    out = true;
                    return true;
                }

                if (normalized == u"false" || normalized == u"0" || normalized == u"no" || normalized == u"off") {
                    out = false;
                    return true;
                }

                return false;
            }

            // 在线性对象存储中查找字段，保持插入顺序结构。
            ConfigObjectEntry* FindObjectEntry(ConfigObject& object, const String& key) {
                for (auto& entry : object) if (entry.key.Equals(key)) return &entry;
                return nullptr;
            }

            // 在线性对象存储中查找只读字段。
            const ConfigObjectEntry* FindObjectEntry(const ConfigObject& object, const String& key) {
                for (const auto& entry : object) if (entry.key.Equals(key)) return &entry;
                return nullptr;
            }

            // 追加 UTF-8 字面量，避免调用点反复构造编码说明。
            void AppendUtf8(String& output, const char* text) {
                output.Append(String(text));
            }

            // 追加 UTF-16 字面量，供序列化器输出固定符号。
            void AppendText(String& output, const char16_t* text) {
                output.Append(String(text));
            }

            // 构造缩进空格串。
            String RepeatSpaces(size_t count) {
                return count == 0 ? String() : String(count, u' ');
            }

            // 输出 JSON 风格四位十六进制转义。
            void AppendHex4(String& output, char32_t value) {
                static constexpr char16_t hex[] = u"0123456789ABCDEF"; // 固定大写十六进制表
                output.Append(u'\\');
                output.Append(u'u');
                output.Append(hex[(value >> 12) & 0xF]);
                output.Append(hex[(value >> 8) & 0xF]);
                output.Append(hex[(value >> 4) & 0xF]);
                output.Append(hex[value & 0xF]);
            }

            // 转义为 JSON 字符串字面量，YAML/TOML 输出也复用这套安全转义。
            String QuoteJsonString(const String& value) {
                String output; // 输出字符串字面量
                output.Append(u'"');

                for (char32_t cp : value) {
                    switch (cp) {
                        case U'"': AppendText(output, u"\\\""); break;
                        case U'\\': AppendText(output, u"\\\\"); break;
                        case U'\b': AppendText(output, u"\\b"); break;
                        case U'\f': AppendText(output, u"\\f"); break;
                        case U'\n': AppendText(output, u"\\n"); break;
                        case U'\r': AppendText(output, u"\\r"); break;
                        case U'\t': AppendText(output, u"\\t"); break;
                        default: if (cp < 0x20) AppendHex4(output, cp); else output.Append(cp); break;
                    }
                }

                output.Append(u'"');
                return output;
            }

            // 使用足够精度输出 double，非有限值按 JSON null 兜底。
            String FormatDouble(double value) {
                if (!std::isfinite(value)) return u"null";

                std::ostringstream stream; // 临时窄字符格式化流
                stream << std::setprecision(17) << value;
                return String(stream.str());
            }

            // 根据 code point 位置计算行列号并拼接错误消息。
            String BuildLineColumnMessage(const std::u32string& text, size_t position, const String& message) {
                size_t line = 1; // 当前行号，从 1 开始
                size_t column = 1; // 当前列号，从 1 开始
                size_t limit = std::min(position, text.size()); // 防止错误位置越过文本尾部

                for (size_t i = 0; i < limit; ++i) {
                    if (text[i] == U'\n') {
                        ++line;
                        column = 1;
                    } else ++column;
                }

                return String::Format(u"line {}, column {}: {}", line, column, message);
            }

            // 去掉行尾注释，忽略单双引号内部的 marker。
            String StripLineComment(const String& line, char32_t marker) {
                std::u32string data = line.ToU32String(); // 当前行 code point 文本
                bool inSingle = false; // 是否处于单引号字符串
                bool inDouble = false; // 是否处于双引号字符串
                bool escaped = false;  // 双引号字符串内转义状态

                for (size_t i = 0; i < data.size(); ++i) {
                    char32_t ch = data[i]; // 当前 code point
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
                    else if (ch == marker) return TrimLineSpace(String(std::u32string_view(data.data(), i)));
                }

                return TrimLineSpace(line);
            }

            // 轻量判断标量是否可能是数字，真正边界由严格解析函数确认。
            bool LooksLikeNumberToken(const String& value) {
                std::u32string text = value.ToU32String(); // 待判断标量文本
                if (text.empty()) return false;

                size_t pos = 0; // 当前扫描位置
                if (text[pos] == U'+' || text[pos] == U'-') ++pos;
                if (pos >= text.size()) return false;

                bool hasDigit = false; // 是否已经见到尾数数字
                while (pos < text.size() && IsAsciiDigit(text[pos])) {
                    hasDigit = true;
                    ++pos;
                }

                if (pos < text.size() && text[pos] == U'.') {
                    ++pos;
                    while (pos < text.size() && IsAsciiDigit(text[pos])) {
                        hasDigit = true;
                        ++pos;
                    }
                }

                if (pos < text.size() && (text[pos] == U'e' || text[pos] == U'E')) {
                    ++pos;
                    if (pos < text.size() && (text[pos] == U'+' || text[pos] == U'-')) ++pos;
                    bool expDigit = false; // 指数部分是否至少有一位数字
                    while (pos < text.size() && IsAsciiDigit(text[pos])) {
                        expDigit = true;
                        ++pos;
                    }
                    if (!expDigit) return false;
                }

                return hasDigit && pos == text.size();
            }

            // 解析 YAML/TOML 共用的简单标量，复杂结构由各自解析器处理。
            ConfigNode ParseSimpleScalar(const String& raw) {
                String value = TrimLineSpace(raw); // 去除行内空白后的标量文本
                if (value.Empty()) return ConfigNode(ConfigText(String()));

                // 双引号字符串优先尝试 JSON 解码，以复用转义和 Unicode 处理。
                if ((value.StartsWith(u"\"") && value.EndsWith(u"\"")) ||
                    (value.StartsWith(u"'") && value.EndsWith(u"'"))) {
                    if (value.StartsWith(u"\"")) {
                        auto json = ConfigValue::TryParseJson(value);
                        if (json.IsOk()) {
                            ConfigValue parsed = json.MoveValue(); // JSON 根只分配一次公开 PImpl
                            return ConfigValueAccess::TakeNode(parsed);
                        }
                    }
                    return ConfigNode(ConfigText(value.SubString(1, value.Size() - 2)));
                }

                String lowered = value.ToLower(); // 小写标量，用于 null/bool 识别
                if (lowered == u"null" || lowered == u"~") return ConfigNode();
                if (lowered == u"true" || lowered == u"yes" || lowered == u"on") return ConfigNode(true);
                if (lowered == u"false" || lowered == u"no" || lowered == u"off") return ConfigNode(false);

                if (LooksLikeNumberToken(value)) {
                    if (value.Find(u".") != String::npos || value.Find(u"e") != String::npos || value.Find(u"E") != String::npos) {
                        double parsed = 0.0; // 严格解析后的浮点标量
                        if (ParseDoubleStrict(value, parsed)) return ConfigNode(parsed);
                    }

                    int64_t parsed = 0; // 严格解析后的整数标量
                    if (ParseInt64Strict(value, parsed)) return ConfigNode(parsed);
                }

                return ConfigNode(ConfigText(std::move(value)));
            }
        }
    }
}
