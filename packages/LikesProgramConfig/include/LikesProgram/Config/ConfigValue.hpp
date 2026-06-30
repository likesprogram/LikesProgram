#pragma once
#include <LikesProgram/Config/system/LikesProgramConfigExport.hpp>
#include <LikesProgram/Core/Result.hpp>
#include <LikesProgram/Core/String.hpp>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace LikesProgram {
    namespace Config {
        // 配置值的实际存储类型，供类型检查、Schema 校验和序列化分支使用。
        enum class ConfigValueType {
            Null,
            String,
            Int64,
            Double,
            Bool,
            Array,
            Object
        };

        // 内部访问桥接结构，只允许包内实现查看 PImpl 存储。
        struct ConfigValueAccess;

        // 通用配置值树，支持标量、数组、对象以及 JSON/YAML/TOML 往返。
        class LIKESPROGRAM_CONFIG_API ConfigValue {
        public:
            // 构造 null 值。
            ConfigValue();

            // 从字符串标量构造配置值。
            explicit ConfigValue(const String& raw);

            // 从 UTF-16 C 字符串构造配置值，空指针按空字符串处理。
            ConfigValue(const char16_t* raw);

            // 从 64 位整数构造配置值。
            ConfigValue(int64_t value);

            // 从普通整数构造配置值。
            ConfigValue(int value);

            // 从双精度浮点构造配置值。
            ConfigValue(double value);

            // 从布尔值构造配置值。
            ConfigValue(bool value);

            // 深拷贝配置值树。
            ConfigValue(const ConfigValue& other);

            // 移动配置值树，源对象保持可析构状态。
            ConfigValue(ConfigValue&& other) noexcept;

            // 释放 PImpl 存储。
            ~ConfigValue();

            // 深拷贝赋值配置值树。
            ConfigValue& operator=(const ConfigValue& other);

            // 移动赋值配置值树。
            ConfigValue& operator=(ConfigValue&& other) noexcept;

            // 创建 null 值。
            static ConfigValue Null();

            // 创建空数组值。
            static ConfigValue Array();

            // 创建空对象值。
            static ConfigValue Object();

            // 返回当前值类型。
            ConfigValueType Type() const;

            // 返回当前值类型的稳定文本名。
            String TypeName() const;

            // 判断当前值是否为 null。
            bool IsNull() const;

            // 判断当前值是否为字符串。
            bool IsString() const;

            // 判断当前值是否为 64 位整数。
            bool IsInt64() const;

            // 判断当前值是否为双精度浮点。
            bool IsDouble() const;

            // 判断当前值是否为布尔值。
            bool IsBool() const;

            // 判断当前值是否为数组。
            bool IsArray() const;

            // 判断当前值是否为对象。
            bool IsObject() const;

            // 判断当前值是否为整数或浮点数。
            bool IsNumber() const;

            // 判断当前值是否为非容器标量。
            bool IsScalar() const;

            // 返回原始字符串标量；非字符串或空对象返回共享空串。
            const String& Raw() const noexcept;

            // 判断 null、空字符串、空数组或空对象。
            bool Empty() const;

            // 按字符串读取值，类型不匹配或空字符串时返回默认值。
            String AsString(const String& defaultValue = String()) const;

            // 按 int64 读取值，字符串会执行严格数字解析。
            int64_t AsInt64(int64_t defaultValue = 0) const;

            // 按 double 读取值，字符串会执行严格浮点解析。
            double AsDouble(double defaultValue = 0.0) const;

            // 按 bool 读取值，支持 true/false/on/off/yes/no 等配置写法。
            bool AsBool(bool defaultValue = false) const;

            // 返回数组元素数或对象字段数。
            size_t Size() const;

            // 判断对象中是否存在指定 key，支持点路径读取。
            bool Contains(const String& key) const;

            // 按 key 或 dotted path 读取对象字段，缺失时返回 null。
            ConfigValue Get(const String& key) const;

            // 按索引读取数组元素，越界时返回 null。
            ConfigValue At(size_t index) const;

            // 设置对象字段，非对象值会先转换为空对象。
            void Set(const String& key, const ConfigValue& value);

            // 移动设置对象字段，解析器热路径可避免深拷贝值树。
            void Set(const String& key, ConfigValue&& value);

            // 追加数组元素，非数组值会先转换为空数组。
            void PushBack(const ConfigValue& value);

            // 移动追加数组元素，解析器热路径可避免深拷贝值树。
            void PushBack(ConfigValue&& value);

            // 删除对象字段，返回是否实际删除。
            bool Remove(const String& key);

            // 返回对象字段名快照，保持插入顺序。
            std::vector<String> Keys() const;

            // 尝试从 JSON 文本解析配置值树。
            static Result<ConfigValue> TryParseJson(const String& text);

            // 从 JSON 文本解析配置值树，失败时抛出异常。
            static ConfigValue FromJson(const String& text);

            // 序列化为 JSON，indent < 0 时输出紧凑格式。
            String ToJson(int indent = 2) const;

            // 尝试从 YAML 文本解析配置值树。
            static Result<ConfigValue> TryParseYaml(const String& text);

            // 从 YAML 文本解析配置值树，失败时抛出异常。
            static ConfigValue FromYaml(const String& text);

            // 序列化为 YAML，indent <= 0 时使用默认缩进。
            String ToYaml(int indent = 2) const;

            // 尝试从 TOML 文本解析配置值树。
            static Result<ConfigValue> TryParseToml(const String& text);

            // 从 TOML 文本解析配置值树，失败时抛出异常。
            static ConfigValue FromToml(const String& text);

            // 序列化为 TOML 文档，非对象根值返回空文档。
            String ToToml() const;

            // 按值树内容比较两个配置值。
            bool operator==(const ConfigValue& other) const;

            // 按值树内容判断两个配置值不相等。
            bool operator!=(const ConfigValue& other) const { return !(*this == other); }

        private:
            struct ConfigValueImpl;
            ConfigValueImpl* m_impl = nullptr; // 唯一拥有的配置值存储对象

            // 懒初始化 PImpl，保证 moved-from 对象可继续使用。
            void EnsureImpl();

            friend struct ConfigValueAccess;
        };
    }
}
