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
#include <variant>

namespace LikesProgram {
    namespace Config {
        namespace Internal {
            // 对象字段前置声明，避免 ConfigValue 存储别名提前依赖完整定义。
            struct ConfigObjectEntry;

            using ConfigArray = std::vector<ConfigValue>; // 数组节点按插入顺序保存子值
            using ConfigObject = std::vector<ConfigObjectEntry>; // 对象节点按插入顺序保存字段
            using ConfigStorage = std::variant<std::monostate, String, int64_t, double, bool, ConfigArray, ConfigObject>; // ConfigValue 的唯一值存储

            // 对象字段条目，保持 key 的原始顺序以便序列化稳定输出。
            struct ConfigObjectEntry {
                String key;        // 字段名，已由写入路径完成空白归一化
                ConfigValue value;  // 字段值，拥有完整子树生命周期

                // 按字段名和值树内容比较，用于 ConfigValue 整树相等判断。
                bool operator==(const ConfigObjectEntry& other) const {
                    return key == other.key && value == other.value;
                }
            };

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
            // 为对象字段预留容量，解析器热路径避免反复扩容。
            void ReserveObject(ConfigValue& value, size_t capacity);
            // 为数组元素预留容量，解析器热路径避免反复扩容。
            void ReserveArray(ConfigValue& value, size_t capacity);

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
            ConfigValue ParseSimpleScalar(const String& raw);

            // 解析完整 JSON 文档为配置值树。
            Result<ConfigValue> ParseJsonDocument(const String& text);
            // 将配置值树序列化为 JSON。
            void SerializeJsonValue(const ConfigValue& value, String& output, int indent, int level);
            // 解析完整 YAML 文档为配置值树。
            Result<ConfigValue> ParseYamlDocument(const String& text);
            // 将配置值树序列化为 YAML。
            void SerializeYamlValue(const ConfigValue& value, String& output, int indent, int level);
            // 解析完整 TOML 文档为配置值树。
            Result<ConfigValue> ParseTomlDocument(const String& text);
            // 将对象根配置值序列化为 TOML 文档。
            String SerializeTomlDocument(const ConfigValue& value);
        }

        struct ConfigValue::ConfigValueImpl {
            Internal::ConfigStorage m_value; // 当前节点实际存储，随 ConfigValue PImpl 生命周期拥有
        };

        // 包内访问桥，集中隔离对 ConfigValue PImpl 的直接访问。
        struct ConfigValueAccess {
            // 返回可写存储，moved-from 值会先懒初始化。
            static Internal::ConfigStorage& Storage(ConfigValue& value) {
                value.EnsureImpl();
                return value.m_impl->m_value;
            }

            // 返回只读存储，空 PImpl 使用共享 null 存储。
            static const Internal::ConfigStorage& Storage(const ConfigValue& value) {
                static const Internal::ConfigStorage nullStorage; // moved-from/空值共享只读 null
                return value.m_impl ? value.m_impl->m_value : nullStorage;
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
        };

        namespace Internal {
            // 按对象插入顺序遍历字段，避免 Keys()+Get() 的重复查找。
            template <typename Callback>
            void ForEachObjectEntry(const ConfigValue& value, Callback&& callback) {
                const auto* object = ConfigValueAccess::Object(value); // 只读遍历对象存储，避免 Keys()+Get() 的重复查找
                if (!object) return;

                for (const auto& entry : *object) {
                    callback(entry.key, entry.value);
                }
            }

            // 按数组索引顺序遍历元素，避免 At() 逐项复制。
            template <typename Callback>
            void ForEachArrayItem(const ConfigValue& value, Callback&& callback) {
                const auto* array = ConfigValueAccess::Array(value); // 只读遍历数组存储，避免 At() 逐项复制
                if (!array) return;

                for (size_t i = 0; i < array->size(); ++i) {
                    callback(i, (*array)[i]);
                }
            }
        }
    }
}
