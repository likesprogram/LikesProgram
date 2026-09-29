#include "LikesProgram/Config/ConfigurationInternal.hpp"
#include <optional>

namespace LikesProgram {
    namespace Config {
        namespace Internal {
            namespace {
                // 预处理后的 YAML 行，保留缩进和原始行号用于错误诊断。
                struct YamlLine {
                    size_t indent = 0; // 行首空格数，禁止 tab 缩进
                    size_t line = 0;   // 原始文档行号，从 1 开始
                    String text;       // 去除缩进和行尾注释后的正文
                };

                // YAML 递归阶段携带 move-only 节点或失败状态，不经过公开 PImpl。
                class YamlNodeResult final {
                public:
                    YamlNodeResult(ConfigNode&& value) : m_value(std::move(value)) { }

                    YamlNodeResult(const Status& status) : m_status(status) { }

                    YamlNodeResult(const YamlNodeResult&) = delete;
                    YamlNodeResult(YamlNodeResult&& other) = default;
                    YamlNodeResult& operator=(const YamlNodeResult&) = delete;
                    YamlNodeResult& operator=(YamlNodeResult&& other) = default;

                    // 成功结果始终持有一个内部节点。
                    bool IsOk() const noexcept { return m_value.has_value(); }
                    // 返回失败状态，调用方只在 !IsOk() 时读取。
                    const Status& GetStatus() const noexcept { return m_status; }
                    // 返回当前节点只读引用，合并对象字段时不克隆。
                    const ConfigNode& Value() const { return *m_value; }
                    // 移出当前节点，父容器按值接管子树。
                    ConfigNode&& MoveValue() { return std::move(*m_value); }

                private:
                    std::optional<ConfigNode> m_value; // 成功分支的递归内部节点
                    Status m_status; // 失败分支的精确 YAML 诊断
                };

                // 写入 YAML 对象字段，重复 key 继续覆盖首次插入位置。
                void SetYamlEntry(ConfigObject& object, const String& key, ConfigNode&& value) {
                    if (auto* entry = FindObjectEntry(object, key)) {
                        entry->value = std::move(value);
                        return;
                    }
                    object.push_back(ConfigObjectEntry{ ConfigText(key), std::move(value) });
                }

                // 过滤空行和注释行，并把缩进信息转成解析器可消费的行表。
                Result<std::vector<YamlLine>> PrepareYamlLines(const String& text) {
                    std::vector<YamlLine> result; // 有效 YAML 行表，后续递归解析按索引推进
                    auto lines = SplitLines(text); // 原始文档行列表，保留空行供行号计算

                    for (size_t i = 0; i < lines.size(); ++i) {
                        std::u32string raw = lines[i].ToU32String(); // 当前行 code point 文本
                        size_t indent = 0; // 当前行缩进宽度，只统计空格
                        while (indent < raw.size() && raw[indent] == U' ') ++indent;
                        if (indent < raw.size() && raw[indent] == U'\t') return Status::InvalidArgument(String::Format(u"YAML parse error at line {}: tab indentation is not supported", i + 1));

                        String body(std::u32string_view(raw.data() + indent, raw.size() - indent)); // 去缩进后的行正文
                        body = StripLineComment(body, U'#');
                        if (body.Empty()) continue;

                        result.push_back(YamlLine{ indent, i + 1, body });
                    }

                    return result;
                }

                // 查找顶层冒号，忽略单双引号内部的冒号。
                size_t FindTopLevelColon(const String& text) {
                    bool inSingle = false; // 当前是否位于单引号字符串
                    bool inDouble = false; // 当前是否位于双引号字符串
                    bool escaped = false;  // 双引号字符串内的转义延迟状态
                    size_t index = 0;      // 当前 code point 位置

                    for (auto ch : text) {
                        if (inDouble) {
                            if (escaped) escaped = false;
                            else if (ch == U'\\') escaped = true;
                            else if (ch == U'"') inDouble = false;
                        } else if (inSingle) { if (ch == U'\'') inSingle = false; }
                        else if (ch == U'"') inDouble = true;
                        else if (ch == U'\'') inSingle = true;
                        else if (ch == U':') return index;
                        ++index;
                    }
                    return String::npos;
                }

                // 读取 YAML 块字符串；folded 模式把换行折叠为空格。
                String ReadYamlBlockString(const std::vector<YamlLine>& lines, size_t& index, size_t parentIndent, bool folded) {
                    String output; // 块字符串输出缓冲
                    bool first = true; // 控制首行前是否追加折叠分隔符

                    while (index < lines.size() && lines[index].indent > parentIndent) {
                        if (!first) output.Append(folded ? u' ' : u'\n');
                        first = false;
                        output.Append(lines[index].text);
                        ++index;
                    }

                    return output;
                }

                YamlNodeResult ParseYamlBlock(
                    const std::vector<YamlLine>& lines, size_t& index, size_t indent);

                // 解析相同缩进层级下的一组 key:value 字段。
                YamlNodeResult ParseYamlObject(
                    const std::vector<YamlLine>& lines, size_t& index, size_t indent) {
                    ConfigObject object; // 当前对象节点，字段按出现顺序保存
                    object.reserve(lines.size() - index);

                    while (index < lines.size()) {
                        const auto& line = lines[index]; // 当前待消费 YAML 行
                        if (line.indent < indent) break;
                        if (line.indent != indent) return Status::InvalidArgument(String::Format(u"YAML parse error at line {}: unexpected indentation", line.line));
                        if (line.text.StartsWith(u"-")) break;

                        size_t colon = FindTopLevelColon(line.text); // 顶层 key/value 分隔符
                        if (colon == String::npos) return Status::InvalidArgument(String::Format(u"YAML parse error at line {}: expected key:value", line.line));

                        String key = TrimLineSpace(line.text.SubString(0, colon)); // 当前字段名
                        String rest = TrimLineSpace(line.text.SubString(colon + 1, line.text.Size() - colon - 1)); // 冒号右侧内容
                        if (key.Empty()) return Status::InvalidArgument(String::Format(u"YAML parse error at line {}: empty key", line.line));

                        ++index;
                        // 空值后跟更深缩进时解析子块；否则按 null 处理。
                        if (rest == u"|" || rest == u">") SetYamlEntry(object, key, ConfigNode(ConfigText(ReadYamlBlockString(lines, index, indent, rest == u">"))));
                        else if (!rest.Empty()) SetYamlEntry(object, key, ParseSimpleScalar(rest));
                        else if (index < lines.size() && lines[index].indent > indent) {
                            auto child = ParseYamlBlock(lines, index, lines[index].indent);
                            if (!child.IsOk()) return child.GetStatus();
                            SetYamlEntry(object, key, child.MoveValue());
                        } else SetYamlEntry(object, key, ConfigNode());
                    }

                    return ConfigNode(std::move(object));
                }

                // 解析相同缩进层级下的一组列表项。
                YamlNodeResult ParseYamlArray(
                    const std::vector<YamlLine>& lines, size_t& index, size_t indent) {
                    ConfigArray array; // 当前数组节点，元素按出现顺序保存
                    array.reserve(lines.size() - index);

                    while (index < lines.size()) {
                        const auto& line = lines[index]; // 当前列表项行
                        if (line.indent < indent) break;
                        if (line.indent != indent || !line.text.StartsWith(u"-")) break;

                        String rest = TrimLineSpace(line.text.SubString(1, line.text.Size() - 1)); // '-' 后的行内内容
                        ++index;

                        if (rest.Empty()) {
                            // 空列表项允许用下一层缩进表达对象或数组子块。
                            if (index < lines.size() && lines[index].indent > indent) {
                                auto child = ParseYamlBlock(lines, index, lines[index].indent);
                                if (!child.IsOk()) return child.GetStatus();
                                array.push_back(child.MoveValue());
                            } else array.push_back(ConfigNode());
                            continue;
                        }

                        size_t colon = FindTopLevelColon(rest); // 行内对象形式的 key/value 分隔符
                        if (colon != String::npos && !(rest.StartsWith(u"\"") || rest.StartsWith(u"'"))) {
                            ConfigObject object; // 当前列表项行内对象字段
                            String key = TrimLineSpace(rest.SubString(0, colon)); // 行内对象首字段名
                            String value = TrimLineSpace(rest.SubString(colon + 1, rest.Size() - colon - 1)); // 首字段值文本
                            SetYamlEntry(object, key, value.Empty() ? ConfigNode() : ParseSimpleScalar(value));

                            if (index < lines.size() && lines[index].indent > indent) {
                                // 行内对象后续缩进字段并入同一个数组元素。
                                auto extra = ParseYamlObject(lines, index, lines[index].indent);
                                if (!extra.IsOk()) return extra.GetStatus();

                                ConfigNode extraValue = extra.MoveValue(); // 待并入的临时对象根
                                auto* extraObject = ConfigValueAccess::Object(extraValue); // 后续缩进字段
                                for (auto& entry : *extraObject) {
                                    if (auto* existing = FindObjectEntry(object, entry.key.AsString())) existing->value = std::move(entry.value);
                                    else object.push_back(ConfigObjectEntry{ std::move(entry.key), std::move(entry.value) });
                                }
                            }
                            array.push_back(ConfigNode(std::move(object)));
                        } else array.push_back(ParseSimpleScalar(rest));
                    }

                    return ConfigNode(std::move(array));
                }

                // 根据当前行首 token 选择对象或数组解析器。
                YamlNodeResult ParseYamlBlock(
                    const std::vector<YamlLine>& lines, size_t& index, size_t indent) {
                    if (index >= lines.size()) return ConfigNode();
                    if (lines[index].indent != indent) return Status::InvalidArgument(String::Format(u"YAML parse error at line {}: unexpected indentation", lines[index].line));
                    if (lines[index].text.StartsWith(u"-")) return ParseYamlArray(lines, index, indent);
                    return ParseYamlObject(lines, index, indent);
                }
            }

            // 解析完整 YAML 文档；空文档按 null 值处理。
            Result<ConfigValue> ParseYamlDocument(const String& text) {
                auto prepared = PrepareYamlLines(text); // 预处理后的行表或错误状态
                if (!prepared.IsOk()) return prepared.GetStatus();

                const auto& lines = prepared.Value(); // 有效 YAML 行表
                if (lines.empty()) return ConfigValue::Null();

                size_t index = 0; // 当前待解析行索引
                auto result = ParseYamlBlock(lines, index, lines.front().indent); // 根节点解析结果
                if (!result.IsOk()) return result.GetStatus();
                if (index != lines.size()) return Status::InvalidArgument(String::Format(u"YAML parse error at line {}: unexpected trailing content", lines[index].line));
                return ConfigValueAccess::FromNode(result.MoveValue());
            }

            // 递归序列化 YAML；对象和数组用缩进表达层级。
            void SerializeYamlValue(const ConfigNode& value, String& output, int indent, int level) {
                String prefix = RepeatSpaces(static_cast<size_t>(level * indent)); // 当前层级行首缩进
                if (NodeIs(value, ConfigValueType::Object)) {
                    ForEachObjectEntry(value, [&](const String& key, const ConfigNode& child) {
                        output.Append(prefix);
                        output.Append(key);
                        if (NodeIs(child, ConfigValueType::Object) ||
                            NodeIs(child, ConfigValueType::Array)) {
                            AppendText(output, u":\n");
                            SerializeYamlValue(child, output, indent, level + 1);
                        } else {
                            AppendText(output, u": ");
                            output.Append(NodeIs(child, ConfigValueType::String) ? QuoteJsonString(NodeString(child)) : SerializeJsonDocument(child, -1));
                            output.Append(u'\n');
                        }
                    });
                    return;
                }

                if (NodeIs(value, ConfigValueType::Array)) {
                    ForEachArrayItem(value, [&](size_t, const ConfigNode& item) {
                        // 复合元素另起下一层，标量元素保持单行输出。
                        output.Append(prefix);
                        AppendText(output, u"-");
                        if (NodeIs(item, ConfigValueType::Object) ||
                            NodeIs(item, ConfigValueType::Array)) {
                            output.Append(u'\n');
                            SerializeYamlValue(item, output, indent, level + 1);
                        } else {
                            output.Append(u' ');
                            output.Append(NodeIs(item, ConfigValueType::String) ? QuoteJsonString(NodeString(item)) : SerializeJsonDocument(item, -1));
                            output.Append(u'\n');
                        }
                    });
                    return;
                }

                output.Append(prefix);
                output.Append(NodeIs(value, ConfigValueType::String) ? QuoteJsonString(NodeString(value)) : SerializeJsonDocument(value, -1));
                output.Append(u'\n');
            }
        }

        Result<ConfigValue> ConfigValue::TryParseYaml(const String& text) {
            // Try* 接口保留错误状态，不抛出异常。
            return Internal::ParseYamlDocument(text);
        }

        ConfigValue ConfigValue::FromYaml(const String& text) {
            auto result = TryParseYaml(text); // 解析结果，失败时转成异常接口
            if (!result.IsOk()) throw std::runtime_error(result.GetStatus().ToString().ToStdString());
            return result.MoveValue();
        }

        String ConfigValue::ToYaml(int indent) const {
            String output; // YAML 序列化输出缓冲
            Internal::SerializeYamlValue(ConfigValueAccess::Node(*this), output, indent > 0 ? indent : 2, 0);
            return output;
        }
    }
}
