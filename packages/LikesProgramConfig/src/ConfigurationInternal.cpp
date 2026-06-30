#include <LikesProgram/Config/ConfigurationInternal.hpp>

namespace LikesProgram {
    namespace Config {
        namespace Internal {
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
                return (ch >= U'0' && ch <= U'9') ||
                    (ch >= U'a' && ch <= U'f') ||
                    (ch >= U'A' && ch <= U'F');
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

                if (begin <= data.size()) {
                    lines.emplace_back(std::u32string_view(data.data() + begin, data.size() - begin));
                }

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
                for (auto cp : key) {
                    if (cp == U'.') return true;
                }
                return false;
            }

            // 将 dotted path 拆为非空片段，连续点会被忽略为空片段。
            std::vector<String> SplitDottedPath(const String& key) {
                std::vector<String> parts; // 输出路径片段
                std::u32string data = key.ToU32String(); // key 的 code point 文本
                size_t begin = 0; // 当前片段起始位置

                for (size_t i = 0; i <= data.size(); ++i) {
                    if (i < data.size() && data[i] != U'.') continue;

                    if (i > begin) {
                        parts.emplace_back(std::u32string_view(data.data() + begin, i - begin));
                    }
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
                if (normalized == u"true" || normalized == u"1" ||
                    normalized == u"yes" || normalized == u"on") {
                    out = true;
                    return true;
                }

                if (normalized == u"false" || normalized == u"0" ||
                    normalized == u"no" || normalized == u"off") {
                    out = false;
                    return true;
                }

                return false;
            }

            // 在线性对象存储中查找字段，保持插入顺序结构。
            ConfigObjectEntry* FindObjectEntry(ConfigObject& object, const String& key) {
                for (auto& entry : object) {
                    if (entry.key == key) return &entry;
                }
                return nullptr;
            }

            // 在线性对象存储中查找只读字段。
            const ConfigObjectEntry* FindObjectEntry(const ConfigObject& object, const String& key) {
                for (const auto& entry : object) {
                    if (entry.key == key) return &entry;
                }
                return nullptr;
            }

            // 为对象节点预留字段空间，非对象会先转换为空对象。
            void ReserveObject(ConfigValue& value, size_t capacity) {
                auto& storage = ConfigValueAccess::Storage(value); // 当前节点底层存储，避免热路径重复解引用 PImpl
                storage = ConfigObject{};
                std::get<ConfigObject>(storage).reserve(capacity);
            }

            // 为数组节点预留元素空间，非数组会先转换为空数组。
            void ReserveArray(ConfigValue& value, size_t capacity) {
                auto& storage = ConfigValueAccess::Storage(value); // 当前节点底层存储，避免热路径重复解引用 PImpl
                storage = ConfigArray{};
                std::get<ConfigArray>(storage).reserve(capacity);
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
                    default:
                        if (cp < 0x20) AppendHex4(output, cp);
                        else output.Append(cp);
                        break;
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
                    }
                    else {
                        ++column;
                    }
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
            ConfigValue ParseSimpleScalar(const String& raw) {
                String value = TrimLineSpace(raw); // 去除行内空白后的标量文本
                if (value.Empty()) return ConfigValue(String());

                // 双引号字符串优先尝试 JSON 解码，以复用转义和 Unicode 处理。
                if ((value.StartsWith(u"\"") && value.EndsWith(u"\"")) ||
                    (value.StartsWith(u"'") && value.EndsWith(u"'"))) {
                    if (value.StartsWith(u"\"")) {
                        auto json = ConfigValue::TryParseJson(value);
                        if (json.IsOk()) return json.Value();
                    }
                    return ConfigValue(value.SubString(1, value.Size() - 2));
                }

                String lowered = value.ToLower(); // 小写标量，用于 null/bool 识别
                if (lowered == u"null" || lowered == u"~") return ConfigValue::Null();
                if (lowered == u"true" || lowered == u"yes" || lowered == u"on") return ConfigValue(true);
                if (lowered == u"false" || lowered == u"no" || lowered == u"off") return ConfigValue(false);

                if (LooksLikeNumberToken(value)) {
                    if (value.Find(u".") != String::npos ||
                        value.Find(u"e") != String::npos ||
                        value.Find(u"E") != String::npos) {
                        double parsed = 0.0; // 严格解析后的浮点标量
                        if (ParseDoubleStrict(value, parsed)) return ConfigValue(parsed);
                    }

                    int64_t parsed = 0; // 严格解析后的整数标量
                    if (ParseInt64Strict(value, parsed)) return ConfigValue(parsed);
                }

                return ConfigValue(value);
            }
        }
    }
}
