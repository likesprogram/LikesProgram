#pragma once
#include <LikesProgram/Config/ConfigSchema.hpp>

namespace LikesProgram {
    namespace Config {
        // 面向用户的配置文档门面，内部以 ConfigValue 对象根节点存储。
        class LIKESPROGRAM_CONFIG_API Configuration {
        public:
            // 创建空对象根配置。
            Configuration();

            // 使用指定值作为配置根节点。
            explicit Configuration(const ConfigValue& root);

            // 移动指定值作为配置根节点，解析热路径可避免深拷贝。
            explicit Configuration(ConfigValue&& root);

            // 深拷贝配置文档。
            Configuration(const Configuration& other);

            // 移动配置文档。
            Configuration(Configuration&& other) noexcept;

            // 释放配置文档存储。
            ~Configuration();

            // 深拷贝赋值配置文档。
            Configuration& operator=(const Configuration& other);

            // 移动赋值配置文档。
            Configuration& operator=(Configuration&& other) noexcept;

            // 返回根节点快照。
            ConfigValue Root() const;

            // 替换根节点，允许对象、数组或标量根。
            void SetRoot(const ConfigValue& value);

            // 移动替换根节点，适合构建器交出完整值树。
            void SetRoot(ConfigValue&& value);

            // 设置字符串配置项。
            void Set(const String& key, const String& value);

            // 设置任意配置值。
            void SetValue(const String& key, const ConfigValue& value);

            // 判断配置项是否存在，支持 dotted path。
            bool Contains(const String& key) const;

            // 读取配置值，缺失时返回 null。
            ConfigValue Get(const String& key) const;

            // 读取字符串配置值，失败时返回默认值。
            String GetString(const String& key, const String& defaultValue = String()) const;

            // 读取 int64 配置值，失败时返回默认值。
            int64_t GetInt64(const String& key, int64_t defaultValue = 0) const;

            // 读取 double 配置值，失败时返回默认值。
            double GetDouble(const String& key, double defaultValue = 0.0) const;

            // 读取 bool 配置值，失败时返回默认值。
            bool GetBool(const String& key, bool defaultValue = false) const;

            // 删除配置项，返回是否实际删除。
            bool Remove(const String& key);

            // 清空配置并恢复为空对象根。
            void Clear();

            // 返回根对象字段数；非对象根返回 0。
            size_t Size() const;

            // 从 key=value 文本构造配置，忽略注释和坏行。
            static Configuration FromKeyValueLines(const String& text);

            // 导出 key=value 文本，嵌套对象以 dotted path 展平。
            String ToKeyValueLines() const;

            // 尝试从 JSON 文档构造配置。
            static Result<Configuration> TryFromJson(const String& text);

            // 从 JSON 文档构造配置，失败时抛出异常。
            static Configuration FromJson(const String& text);

            // 导出 JSON 文档，indent < 0 时输出紧凑格式。
            String ToJson(int indent = 2) const;

            // 尝试从 YAML 文档构造配置。
            static Result<Configuration> TryFromYaml(const String& text);

            // 从 YAML 文档构造配置，失败时抛出异常。
            static Configuration FromYaml(const String& text);

            // 导出 YAML 文档。
            String ToYaml(int indent = 2) const;

            // 尝试从 TOML 文档构造配置。
            static Result<Configuration> TryFromToml(const String& text);

            // 从 TOML 文档构造配置，失败时抛出异常。
            static Configuration FromToml(const String& text);

            // 导出 TOML 文档。
            String ToToml() const;

            // 使用 Schema 校验当前配置。
            ConfigValidationResult Validate(const ConfigSchema& schema) const;

            // 使用 Schema 默认值规则补齐当前配置。
            void ApplyDefaults(const ConfigSchema& schema);

        private:
            struct ConfigurationImpl;
            ConfigurationImpl* m_impl = nullptr; // 唯一拥有的配置文档存储

            // 懒初始化 PImpl，并保证默认根为对象。
            void EnsureImpl();
        };
    }

    using Configuration = Config::Configuration;
}
