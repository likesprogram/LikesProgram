#pragma once
#include <LikesProgram/Config/ConfigValue.hpp>
#include <cstddef>

namespace LikesProgram {
    namespace Config {
        // Schema 约束类型，和 ConfigValueType 分离以支持 Any/Number 等抽象匹配。
        enum class ConfigSchemaType {
            Any,
            Null,
            String,
            Int64,
            Double,
            Bool,
            Number,
            Array,
            Object
        };

        // Schema 校验结果，聚合所有错误而不是遇到第一处就返回。
        class LIKESPROGRAM_CONFIG_API ConfigValidationResult {
        public:
            // 创建空错误集合，表示校验成功。
            ConfigValidationResult();

            // 拷贝校验错误集合。
            ConfigValidationResult(const ConfigValidationResult& other);

            // 移动校验错误集合。
            ConfigValidationResult(ConfigValidationResult&& other) noexcept;

            // 释放错误集合存储。
            ~ConfigValidationResult();

            // 拷贝赋值校验错误集合。
            ConfigValidationResult& operator=(const ConfigValidationResult& other);

            // 移动赋值校验错误集合。
            ConfigValidationResult& operator=(ConfigValidationResult&& other) noexcept;

            // 判断是否没有校验错误。
            bool IsOk() const;

            // 返回聚合错误数量。
            size_t ErrorCount() const;

            // 返回指定位置的错误文本，越界时返回空串。
            String Error(size_t index) const;

            // 返回以换行拼接的完整错误报告。
            String Report() const;

            // 追加一条错误，空错误会被忽略。
            void AddError(const String& error);

        private:
            struct ConfigValidationResultImpl;
            ConfigValidationResultImpl* m_impl = nullptr; // 唯一拥有的错误集合存储

            // 懒初始化 PImpl，保证移动后对象可继续追加错误。
            void EnsureImpl();
        };

        // 配置 Schema 构建器，支持嵌套对象、数组元素和默认值注入。
        class LIKESPROGRAM_CONFIG_API ConfigSchema {
        public:
            // 创建 Any Schema。
            ConfigSchema();

            // 深拷贝 Schema 规则树。
            ConfigSchema(const ConfigSchema& other);

            // 移动 Schema 规则树。
            ConfigSchema(ConfigSchema&& other) noexcept;

            // 释放 Schema 规则树。
            ~ConfigSchema();

            // 深拷贝赋值 Schema 规则树。
            ConfigSchema& operator=(const ConfigSchema& other);

            // 移动赋值 Schema 规则树。
            ConfigSchema& operator=(ConfigSchema&& other) noexcept;

            // 创建允许任意类型的 Schema。
            static ConfigSchema Any();

            // 创建指定类型的 Schema。
            static ConfigSchema Type(ConfigSchemaType type);

            // 创建 null 类型 Schema。
            static ConfigSchema NullType();

            // 创建字符串类型 Schema。
            static ConfigSchema StringType();

            // 创建 int64 类型 Schema。
            static ConfigSchema Int64Type();

            // 创建 double 类型 Schema。
            static ConfigSchema DoubleType();

            // 创建 bool 类型 Schema。
            static ConfigSchema BoolType();

            // 创建整数或浮点均可接受的数字 Schema。
            static ConfigSchema NumberType();

            // 创建数组类型 Schema，不限制元素类型。
            static ConfigSchema ArrayType();

            // 创建数组类型 Schema，并约束每个元素。
            static ConfigSchema ArrayType(const ConfigSchema& itemSchema);

            // 创建对象类型 Schema。
            static ConfigSchema ObjectType();

            // 添加必填字段约束，并将当前 Schema 标记为对象。
            ConfigSchema& Required(const String& key, const ConfigSchema& schema);

            // 添加可选字段约束，可附带默认值注入规则。
            ConfigSchema& Optional(const String& key, const ConfigSchema& schema,
                const ConfigValue& defaultValue = ConfigValue());

            // 设置数组元素 Schema，并将当前 Schema 标记为数组。
            ConfigSchema& Items(const ConfigSchema& itemSchema);

            // 设置对象是否允许未知字段。
            ConfigSchema& AllowUnknownKeys(bool allow);

            // 校验配置值并返回完整错误集合。
            ConfigValidationResult Validate(const ConfigValue& value) const;

            // 按可选字段默认值规则修改配置值。
            void ApplyDefaults(ConfigValue& value) const;

        private:
            struct ConfigSchemaImpl;
            ConfigSchemaImpl* m_impl = nullptr; // 唯一拥有的 Schema 规则树

            // 懒初始化 PImpl，保证移动后对象可重新配置。
            void EnsureImpl();

            // 递归校验当前节点，path 用于构建用户可读错误路径。
            void ValidateRecursive(const ConfigValue& value, const String& path,
                ConfigValidationResult& result) const;

            // 递归注入默认值，只修改对象节点。
            void ApplyDefaultsRecursive(ConfigValue& value) const;
        };
    }
}
