#pragma once

#include <LikesProgram/Config/Config.hpp>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <variant>

namespace LikesProgram {
    namespace Config {
        namespace Internal {
            // 递归节点与对象字段前置声明，容器负责延后要求完整类型。
            struct ConfigNode;
            struct ConfigObjectEntry;

            // JSON5 可先保留原生 UTF-16 文本，公开读取时再一次性物化 Core/String。
            class ConfigText final {
            public:
                ConfigText(const String& value);
                ConfigText(String&& value);
                ConfigText(std::u16string_view value);
                ConfigText(std::u16string&& value);
                ConfigText(const ConfigText& other);
                ConfigText(ConfigText&& other);
                ~ConfigText();
                ConfigText& operator=(const ConfigText& other);
                ConfigText& operator=(ConfigText&& other);

                // 返回无分配 UTF-16 view，JSON5 热路径和 key 比较直接使用。
                std::u16string_view View() const noexcept;
                // 返回稳定 Core/String 引用，UTF-16 分支首次读取时转换并缓存。
                const String& AsString() const;
                // 与公开 String key 比较，不触发物化。
                bool Equals(const String& value) const noexcept;
                // 按 UTF-16 内容比较，忽略当前是否已物化。
                bool operator==(const ConfigText& other) const noexcept;

            private:
                static constexpr size_t InlineCapacity = 8; // 与现有紧凑布局基线保持相同内联容量
                static constexpr unsigned char HeapState = 12;
                static constexpr unsigned char OriginalStringState = 13;
                static constexpr unsigned char MaterializedStringState = 14;
                static constexpr size_t StorageBytes = 24;
                static constexpr size_t StateOffset = InlineCapacity * sizeof(char16_t);

                union TextStorage {
                    char16_t inlineData[InlineCapacity];
                    struct HeapText {
                        char16_t* data;
                        size_t length;
                    } heapText;
                    String stringValue;
                    String* materializedValue;
                    unsigned char raw[StorageBytes];

                    TextStorage() noexcept { }
                    ~TextStorage() { }
                };

                static_assert(sizeof(String) <= StateOffset, "String storage must not overlap the packed ConfigText state byte");
                static_assert(sizeof(TextStorage) == StorageBytes, "ConfigText storage must keep the audited 24-byte representation");

                mutable TextStorage m_storage;

                // 从 UTF-16 view 初始化紧凑存储。
                void Assign(std::u16string_view value);
                // 返回当前紧凑存储起点。
                const char16_t* Data() const noexcept;
                // 状态字节位于 inline 数据之后，不占用额外对象空间。
                unsigned char State() const noexcept;
                void SetState(unsigned char state) const noexcept;
                // 释放当前拥有的长文本/字符串资源；仅由终态析构调用。
                void Release() noexcept;
            };

            static_assert(sizeof(ConfigText) == 24, "ConfigText must keep the audited compact size class");

            using ConfigArray = std::vector<ConfigNode>; // 数组节点按插入顺序内联拥有子节点
            using ConfigObject = std::vector<ConfigObjectEntry>; // 对象节点按插入顺序保存字段
            using ConfigStorage = std::variant<std::monostate, ConfigText, int64_t, double, bool, ConfigArray, ConfigObject>; // ConfigValue 的唯一值存储

            // 私有递归节点按值拥有完整子树，只有公开根继续分配 PImpl。
            struct ConfigNode final {
                ConfigNode();
                explicit ConfigNode(ConfigText&& value);
                explicit ConfigNode(int64_t value) noexcept;
                explicit ConfigNode(double value) noexcept;
                explicit ConfigNode(bool value) noexcept;
                explicit ConfigNode(ConfigArray&& value);
                explicit ConfigNode(ConfigObject&& value);
                ConfigNode(const ConfigNode&) = delete;
                ConfigNode(ConfigNode&& other);
                ~ConfigNode();
                ConfigNode& operator=(const ConfigNode&) = delete;
                ConfigNode& operator=(ConfigNode&& other);

                // 按完整树内容比较，数组和对象继续保持插入顺序语义。
                bool operator==(const ConfigNode& other) const;

                ConfigStorage m_value; // 当前节点唯一存储，容器分支递归拥有后代
            };

            // 对象字段条目，保持 key 的原始顺序以便序列化稳定输出。
            struct ConfigObjectEntry {
                ConfigText key;    // 字段名，JSON5 可延迟物化 Core/String
                ConfigNode value;  // 字段值，由当前对象树按值拥有

                // 按字段名和值树内容比较，用于 ConfigValue 整树相等判断。
                bool operator==(const ConfigObjectEntry& other) const {
                    return key == other.key && value == other.value;
                }
            };

            static_assert(!std::is_copy_constructible_v<ConfigNode>, "ConfigNode must remain move-only so vector growth never deep-copies subtrees");
            static_assert(sizeof(ConfigNode) == sizeof(ConfigStorage), "ConfigNode must not add state beyond its single storage member");
            static_assert(sizeof(ConfigObjectEntry) <= sizeof(ConfigText) + sizeof(ConfigNode) + alignof(ConfigNode), "ConfigObjectEntry must contain only key, node and unavoidable alignment");
#if !defined(_DEBUG)
            static_assert(sizeof(ConfigNode) <= 32, "Release ConfigNode must remain in the audited storage size class");
            static_assert(sizeof(ConfigObjectEntry) <= 64, "Release ConfigObjectEntry must keep the audited recursive-node size class");
#endif

            // 显式深克隆节点树，公开复制/Get/At 由此保持脱离父树的值语义。
            ConfigNode CloneNode(const ConfigNode& source);
            // 返回内部节点类型，不创建公开 ConfigValue 包装。
            ConfigValueType NodeType(const ConfigNode& value) noexcept;
            // 判断内部节点是否为指定类型。
            bool NodeIs(const ConfigNode& value, ConfigValueType type) noexcept;
            // 返回内部字符串节点的稳定 Core/String；非字符串返回共享空串。
            const String& NodeString(const ConfigNode& value);

            // 判断 JSON/YAML/TOML 共同使用的 ASCII 空白字符。
            bool IsAsciiSpace(char32_t ch) noexcept;
            // 判断行内空白，不跨越换行边界。
            bool IsLineSpace(char32_t ch) noexcept;
            // 判断配置语法中的十进制 ASCII 数字。
            bool IsAsciiDigit(char32_t ch) noexcept;
            // 判断 Unicode 转义中允许的十六进制数字。
            bool IsHexDigit(char32_t ch) noexcept;
            // 将十六进制字符转换为数值，非法字符返回 -1。
            int HexValue(char32_t ch);

            // 去除首尾 ASCII 空白，保留中间内容。
            String TrimAscii(const String& value);
            // 去除首尾行内空白，保留换行语义外的文本。
            String TrimLineSpace(const String& value);
            // 按 LF/CRLF 拆分配置文本，保留末尾空行语义。
            std::vector<String> SplitLines(const String& text);
            // 查找 key=value 行中的第一个等号。
            size_t FindEquals(const String& line);
            // 判断 key 是否包含 dotted path 分隔符。
            bool HasDot(const String& key);
            // 将 dotted path 拆成非空路径片段。
            std::vector<String> SplitDottedPath(const String& key);

            // 严格解析 int64，要求整段文本都被消费。
            bool ParseInt64Strict(const String& value, int64_t& out);
            // 严格解析 double，拒绝 NaN/Inf 和尾随垃圾。
            bool ParseDoubleStrict(const String& value, double& out);
            // 严格解析常见配置布尔文本，无法识别时返回 false。
            bool ParseBoolStrict(const String& value, bool& out);

            // 在对象中查找可修改字段，返回 nullptr 表示不存在。
            ConfigObjectEntry* FindObjectEntry(ConfigObject& object, const String& key);
            // 在对象中查找只读字段，返回 nullptr 表示不存在。
            const ConfigObjectEntry* FindObjectEntry(const ConfigObject& object, const String& key);
            // 按 UTF-8 字面量追加到 String 输出缓冲。
            void AppendUtf8(String& output, const char* text);
            // 按 UTF-16 字面量追加到 String 输出缓冲。
            void AppendText(String& output, const char16_t* text);
            // 生成指定数量空格，用于缩进序列化。
            String RepeatSpaces(size_t count);
            // 写入 JSON 风格 \uXXXX 转义。
            void AppendHex4(String& output, char32_t value);
            // 生成 JSON 字符串字面量，复用于 JSON/YAML/TOML 输出。
            String QuoteJsonString(const String& value);
            // 以稳定精度格式化浮点数，非有限值输出 null。
            String FormatDouble(double value);
            // 为解析错误构造行列号诊断文本。
            String BuildLineColumnMessage(const std::u32string& text, size_t position, const String& message);
            // 删除行尾注释，忽略引号内部的 marker。
            String StripLineComment(const String& line, char32_t marker);
            // 轻量判断标量文本是否形似数字。
            bool LooksLikeNumberToken(const String& value);
            // 解析 YAML/TOML 共用的简单标量。
            ConfigNode ParseSimpleScalar(const String& raw);

            // 解析完整 JSON5 文档为配置值树。
            Result<ConfigValue> ParseJsonDocument(const String& text);
            // 将配置值树序列化为 JSON5。
            void SerializeJsonValue(const ConfigNode& value, String& output, int indent, int level);
            // 将配置值树直接序列化为完整 JSON5 文档。
            String SerializeJsonDocument(const ConfigNode& value, int indent);
            // 解析完整 YAML 文档为配置值树。
            Result<ConfigValue> ParseYamlDocument(const String& text);
            // 将配置值树序列化为 YAML。
            void SerializeYamlValue(const ConfigNode& value, String& output, int indent, int level);
            // 解析完整 TOML 文档为配置值树。
            Result<ConfigValue> ParseTomlDocument(const String& text);
            // 将对象根配置值序列化为 TOML 文档。
            String SerializeTomlDocument(const ConfigNode& value);

        }

        struct ConfigValue::ConfigValueImpl {
            Internal::ConfigNode m_value; // 公开根节点，随唯一 PImpl 生命周期拥有完整内部树
        };

        static_assert(sizeof(ConfigValue) == sizeof(void*), "ConfigValue public ABI must remain one PImpl pointer");

        // 包内访问桥，集中隔离对 ConfigValue PImpl 的直接访问。
        struct ConfigValueAccess {
            // 返回可写存储，moved-from 值会先懒初始化。
            static Internal::ConfigStorage& Storage(ConfigValue& value) {
                value.EnsureImpl();
                return value.m_impl->m_value.m_value;
            }

            // 返回只读存储，空 PImpl 使用共享 null 存储。
            static const Internal::ConfigStorage& Storage(const ConfigValue& value) {
                static const Internal::ConfigStorage nullStorage; // moved-from/空值共享只读 null
                return value.m_impl ? value.m_impl->m_value.m_value : nullStorage;
            }

            // 返回可写根节点，moved-from 值会先恢复为 null 根。
            static Internal::ConfigNode& Node(ConfigValue& value) {
                value.EnsureImpl();
                return value.m_impl->m_value;
            }

            // 返回只读根节点，moved-from 值使用共享 null 节点。
            static const Internal::ConfigNode& Node(const ConfigValue& value) {
                static const Internal::ConfigNode nullNode; // 共享只读 null 根
                return value.m_impl ? value.m_impl->m_value : nullNode;
            }

            // 将内部节点封装为拥有独立 PImpl 的公开值。
            static ConfigValue FromNode(Internal::ConfigNode&& node) {
                return ConfigValue(new ConfigValue::ConfigValueImpl{ std::move(node) });
            }

            // JSON5 根字符串从 UTF-16 view 直接建立唯一公开根。
            static ConfigValue FromText(std::u16string_view text) {
                return ConfigValue(new ConfigValue::ConfigValueImpl{
                    Internal::ConfigNode(Internal::ConfigText(text)) });
            }

            // 已解码根字符串直接移动到唯一公开根。
            static ConfigValue FromText(std::u16string&& text) {
                return ConfigValue(new ConfigValue::ConfigValueImpl{
                    Internal::ConfigNode(Internal::ConfigText(std::move(text))) });
            }

            // 从公开临时值移出根节点，并把源恢复为可继续使用的 null。
            static Internal::ConfigNode TakeNode(ConfigValue& value) {
                value.EnsureImpl();
                Internal::ConfigNode node(std::move(value.m_impl->m_value)); // 接管完整内部树
                value.m_impl->m_value = Internal::ConfigNode();
                return node;
            }

            // 若当前值为对象，返回可写对象存储。
            static Internal::ConfigObject* Object(ConfigValue& value) {
                return std::get_if<Internal::ConfigObject>(&Storage(value));
            }

            // 若当前值为对象，返回只读对象存储。
            static const Internal::ConfigObject* Object(const ConfigValue& value) {
                return std::get_if<Internal::ConfigObject>(&Storage(value));
            }

            // 若当前值为数组，返回可写数组存储。
            static Internal::ConfigArray* Array(ConfigValue& value) {
                return std::get_if<Internal::ConfigArray>(&Storage(value));
            }

            // 若当前值为数组，返回只读数组存储。
            static const Internal::ConfigArray* Array(const ConfigValue& value) {
                return std::get_if<Internal::ConfigArray>(&Storage(value));
            }

            // 返回内部节点的可写对象分支。
            static Internal::ConfigObject* Object(Internal::ConfigNode& value) {
                return std::get_if<Internal::ConfigObject>(&value.m_value);
            }

            // 返回内部节点的只读对象分支。
            static const Internal::ConfigObject* Object(const Internal::ConfigNode& value) {
                return std::get_if<Internal::ConfigObject>(&value.m_value);
            }

            // 返回内部节点的可写数组分支。
            static Internal::ConfigArray* Array(Internal::ConfigNode& value) {
                return std::get_if<Internal::ConfigArray>(&value.m_value);
            }

            // 返回内部节点的只读数组分支。
            static const Internal::ConfigArray* Array(const Internal::ConfigNode& value) {
                return std::get_if<Internal::ConfigArray>(&value.m_value);
            }
        };

        namespace Internal {
            // 按对象插入顺序遍历字段，避免 Keys()+Get() 的重复查找。
            template <typename Callback>
            void ForEachObjectEntry(const ConfigNode& value, Callback&& callback) {
                const auto* object = ConfigValueAccess::Object(value); // 只读遍历内部对象，不克隆公开子值
                if (!object) return;

                for (const auto& entry : *object) callback(entry.key.AsString(), entry.value);
            }

            // 按数组索引顺序遍历元素，避免 At() 逐项复制。
            template <typename Callback>
            void ForEachArrayItem(const ConfigNode& value, Callback&& callback) {
                const auto* array = ConfigValueAccess::Array(value); // 只读遍历内部数组，不克隆公开子值
                if (!array) return;

                for (size_t i = 0; i < array->size(); ++i) callback(i, (*array)[i]);
            }
        }
    }
}
