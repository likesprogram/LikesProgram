#include "LikesProgram/Config/ConfigurationInternal.hpp"

namespace LikesProgram {
    namespace Config {
        namespace {
            // 判断实际配置节点是否满足 schema 声明的基础类型。
            bool TypeMatches(ConfigSchemaType expected, const Internal::ConfigNode& value) {
                switch (expected) {
                    case ConfigSchemaType::Any: return true;
                    case ConfigSchemaType::Null: return Internal::NodeIs(value, ConfigValueType::Null);
                    case ConfigSchemaType::String: return Internal::NodeIs(value, ConfigValueType::String);
                    case ConfigSchemaType::Int64: return Internal::NodeIs(value, ConfigValueType::Int64);
                    case ConfigSchemaType::Double: return Internal::NodeIs(value, ConfigValueType::Double);
                    case ConfigSchemaType::Bool: return Internal::NodeIs(value, ConfigValueType::Bool);
                    case ConfigSchemaType::Number: return Internal::NodeIs(value, ConfigValueType::Int64) || Internal::NodeIs(value, ConfigValueType::Double);
                    case ConfigSchemaType::Array: return Internal::NodeIs(value, ConfigValueType::Array);
                    case ConfigSchemaType::Object: return Internal::NodeIs(value, ConfigValueType::Object);
                }
                return false;
            }

            // 将内部节点类型转为公开诊断中的稳定名称。
            String NodeTypeName(const Internal::ConfigNode& value) {
                switch (Internal::NodeType(value)) {
                    case ConfigValueType::Null: return u"null";
                    case ConfigValueType::String: return u"string";
                    case ConfigValueType::Int64: return u"int64";
                    case ConfigValueType::Double: return u"double";
                    case ConfigValueType::Bool: return u"bool";
                    case ConfigValueType::Array: return u"array";
                    case ConfigValueType::Object: return u"object";
                }
                return u"unknown";
            }

            // 按 ConfigValue Get/Contains 规则借用对象字段，直接 key 优先于 dotted path。
            const Internal::ConfigNode* FindSchemaNode(const Internal::ConfigNode& value, const String& key) {
                const auto* object = ConfigValueAccess::Object(value); // 当前对象分支
                if (!object) return nullptr;
                if (const auto* entry = Internal::FindObjectEntry(*object, key)) return &entry->value;
                if (!Internal::HasDot(key)) return nullptr;

                const Internal::ConfigNode* current = &value; // dotted path 当前节点
                for (const auto& part : Internal::SplitDottedPath(key)) {
                    const auto* currentObject = ConfigValueAccess::Object(*current);
                    if (!currentObject) return nullptr;
                    const auto* entry = Internal::FindObjectEntry(*currentObject, part); // 当前字段
                    if (!entry) return nullptr;
                    current = &entry->value;
                }
                return current;
            }

            // 可写查找与只读规则相同，用于默认值直接写入内部树。
            Internal::ConfigNode* FindSchemaNode(Internal::ConfigNode& value, const String& key) {
                return const_cast<Internal::ConfigNode*>(FindSchemaNode(static_cast<const Internal::ConfigNode&>(value), key));
            }

            // 将 schema 类型转为错误报告中的稳定小写名称。
            String SchemaTypeName(ConfigSchemaType type) {
                switch (type) {
                    case ConfigSchemaType::Any: return u"any";
                    case ConfigSchemaType::Null: return u"null";
                    case ConfigSchemaType::String: return u"string";
                    case ConfigSchemaType::Int64: return u"int64";
                    case ConfigSchemaType::Double: return u"double";
                    case ConfigSchemaType::Bool: return u"bool";
                    case ConfigSchemaType::Number: return u"number";
                    case ConfigSchemaType::Array: return u"array";
                    case ConfigSchemaType::Object: return u"object";
                }
                return u"unknown";
            }
        }

        struct ConfigValidationResult::ConfigValidationResultImpl {
            std::vector<String> m_errors; // 校验过程中按发现顺序收集的错误文本
        };

        // 创建空校验结果，默认表示校验通过。
        ConfigValidationResult::ConfigValidationResult() : m_impl(new ConfigValidationResultImpl{}) { }

        // 深拷贝错误列表，避免结果对象之间共享可变状态。
        ConfigValidationResult::ConfigValidationResult(const ConfigValidationResult& other) : m_impl(new ConfigValidationResultImpl{}) {
            if (other.m_impl) m_impl->m_errors = other.m_impl->m_errors;
        }

        // 移动接管结果实现对象，源对象置空后仍可析构。
        ConfigValidationResult::ConfigValidationResult(ConfigValidationResult&& other) noexcept : m_impl(other.m_impl) {
            other.m_impl = nullptr;
        }

        // 释放错误列表实现对象。
        ConfigValidationResult::~ConfigValidationResult() {
            delete m_impl;
            m_impl = nullptr;
        }

        // 拷贝赋值时保留目标对象可用的 PImpl，不共享错误列表。
        ConfigValidationResult& ConfigValidationResult::operator=(const ConfigValidationResult& other) {
            if (this == &other) return *this;
            EnsureImpl();
            m_impl->m_errors = other.m_impl ? other.m_impl->m_errors : std::vector<String>{};
            return *this;
        }

        // 移动赋值直接转移 PImpl，释放目标旧内容。
        ConfigValidationResult& ConfigValidationResult::operator=(ConfigValidationResult&& other) noexcept {
            if (this == &other) return *this;
            delete m_impl;
            m_impl = other.m_impl;
            other.m_impl = nullptr;
            return *this;
        }

        // 延迟创建实现对象，支持 moved-from 对象重新使用。
        void ConfigValidationResult::EnsureImpl() {
            if (!m_impl) m_impl = new ConfigValidationResultImpl{};
        }

        // 没有错误即视为通过，空 PImpl 也表示空结果。
        bool ConfigValidationResult::IsOk() const {
            return !m_impl || m_impl->m_errors.empty();
        }

        // 返回当前错误数量，供调用方决定是否输出详细报告。
        size_t ConfigValidationResult::ErrorCount() const {
            return m_impl ? m_impl->m_errors.size() : 0;
        }

        // 按发现顺序读取单条错误，越界返回空字符串。
        String ConfigValidationResult::Error(size_t index) const {
            if (!m_impl || index >= m_impl->m_errors.size()) return String();
            return m_impl->m_errors[index];
        }

        // 将所有错误拼接为多行文本，便于日志或测试断言输出。
        String ConfigValidationResult::Report() const {
            String report; // 多行错误报告输出缓冲
            if (!m_impl) return report;
            for (size_t i = 0; i < m_impl->m_errors.size(); ++i) {
                if (i > 0) report.Append(u'\n');
                report.Append(m_impl->m_errors[i]);
            }
            return report;
        }

        // 追加非空错误文本，调用方负责提供已经格式化好的路径信息。
        void ConfigValidationResult::AddError(const String& error) {
            EnsureImpl();
            if (!error.Empty()) m_impl->m_errors.push_back(error);
        }

        struct ConfigSchema::ConfigSchemaImpl {
            struct FieldRule {
                String m_key; // 对象字段名，不包含父路径
                std::shared_ptr<ConfigSchema> m_schema; // 字段对应的子 schema
                bool m_required = false; // 缺失时是否产生 required 错误
                bool m_hasDefault = false; // ApplyDefaults 是否应写入默认值
                ConfigValue m_defaultValue; // 可选字段缺失时写入的默认节点
            };

            ConfigSchemaType m_type = ConfigSchemaType::Any; // 当前节点期望的基础类型
            bool m_allowUnknownKeys = true; // 对象校验是否允许未声明字段
            bool m_hasItemSchema = false; // 数组元素 schema 是否已配置
            std::shared_ptr<ConfigSchema> m_itemSchema; // 数组元素递归校验规则
            std::vector<FieldRule> m_fields; // 对象字段规则，保持用户声明顺序
        };

        // 构造 Any schema，默认不限制类型。
        ConfigSchema::ConfigSchema() : m_impl(new ConfigSchemaImpl{}) { }

        // 深拷贝 schema 规则，字段子 schema 通过 shared_ptr 保持值语义快照。
        ConfigSchema::ConfigSchema(const ConfigSchema& other) : m_impl(new ConfigSchemaImpl{}) {
            if (other.m_impl) *m_impl = *other.m_impl;
        }

        // 移动接管 schema 实现对象，源对象置空后保持可析构。
        ConfigSchema::ConfigSchema(ConfigSchema&& other) noexcept : m_impl(other.m_impl) {
            other.m_impl = nullptr;
        }

        // 释放 schema 规则实现对象。
        ConfigSchema::~ConfigSchema() {
            delete m_impl;
            m_impl = nullptr;
        }

        // 拷贝赋值，复用目标 PImpl 并复制所有规则字段。
        ConfigSchema& ConfigSchema::operator=(const ConfigSchema& other) {
            if (this == &other) return *this;
            EnsureImpl();
            if (other.m_impl) *m_impl = *other.m_impl;
            return *this;
        }

        // 移动赋值，释放目标旧实现后接管源对象。
        ConfigSchema& ConfigSchema::operator=(ConfigSchema&& other) noexcept {
            if (this == &other) return *this;
            delete m_impl;
            m_impl = other.m_impl;
            other.m_impl = nullptr;
            return *this;
        }

        // 确保 moved-from schema 后续仍可继续配置。
        void ConfigSchema::EnsureImpl() {
            if (!m_impl) m_impl = new ConfigSchemaImpl{};
        }

        // 创建不限制类型和值的 schema。
        ConfigSchema ConfigSchema::Any() {
            return ConfigSchema();
        }

        // 创建指定基础类型 schema，后续可继续叠加字段或数组规则。
        ConfigSchema ConfigSchema::Type(ConfigSchemaType type) {
            ConfigSchema schema; // 待返回的 schema 对象
            schema.m_impl->m_type = type;
            return schema;
        }

        // 创建只接受 null 的 schema。
        ConfigSchema ConfigSchema::NullType() { return Type(ConfigSchemaType::Null); }
        // 创建只接受 string 的 schema。
        ConfigSchema ConfigSchema::StringType() { return Type(ConfigSchemaType::String); }
        // 创建只接受 int64 的 schema。
        ConfigSchema ConfigSchema::Int64Type() { return Type(ConfigSchemaType::Int64); }
        // 创建只接受 double 的 schema。
        ConfigSchema ConfigSchema::DoubleType() { return Type(ConfigSchemaType::Double); }
        // 创建只接受 bool 的 schema。
        ConfigSchema ConfigSchema::BoolType() { return Type(ConfigSchemaType::Bool); }
        // 创建接受 int64 或 double 的 schema。
        ConfigSchema ConfigSchema::NumberType() { return Type(ConfigSchemaType::Number); }

        // 创建数组 schema，暂不限制元素类型。
        ConfigSchema ConfigSchema::ArrayType() {
            return Type(ConfigSchemaType::Array);
        }

        // 创建数组 schema 并立即配置元素校验规则。
        ConfigSchema ConfigSchema::ArrayType(const ConfigSchema& itemSchema) {
            ConfigSchema schema = Type(ConfigSchemaType::Array); // 待返回的数组 schema
            schema.Items(itemSchema);
            return schema;
        }

        // 创建对象 schema，默认允许未知字段。
        ConfigSchema ConfigSchema::ObjectType() {
            return Type(ConfigSchemaType::Object);
        }

        // 声明必填字段，缺失时 Validate 会产生错误。
        ConfigSchema& ConfigSchema::Required(const String& key, const ConfigSchema& schema) {
            EnsureImpl();
            m_impl->m_fields.push_back(ConfigSchemaImpl::FieldRule{ key, std::make_shared<ConfigSchema>(schema), true, false, ConfigValue() });
            m_impl->m_type = ConfigSchemaType::Object;
            return *this;
        }

        // 声明可选字段，并在非 null 默认值存在时支持 ApplyDefaults 写入。
        ConfigSchema& ConfigSchema::Optional(const String& key, const ConfigSchema& schema, const ConfigValue& defaultValue) {
            EnsureImpl();
            m_impl->m_fields.push_back(ConfigSchemaImpl::FieldRule{ key, std::make_shared<ConfigSchema>(schema), false, !defaultValue.IsNull(), defaultValue });
            m_impl->m_type = ConfigSchemaType::Object;
            return *this;
        }

        // 配置数组元素 schema，Validate 会逐项递归检查。
        ConfigSchema& ConfigSchema::Items(const ConfigSchema& itemSchema) {
            EnsureImpl();
            m_impl->m_itemSchema = std::make_shared<ConfigSchema>(itemSchema);
            m_impl->m_hasItemSchema = true;
            m_impl->m_type = ConfigSchemaType::Array;
            return *this;
        }

        // 设置对象是否允许 schema 未声明的额外字段。
        ConfigSchema& ConfigSchema::AllowUnknownKeys(bool allow) {
            EnsureImpl();
            m_impl->m_allowUnknownKeys = allow;
            return *this;
        }

        // 对配置值执行完整递归校验并返回错误集合。
        ConfigValidationResult ConfigSchema::Validate(const ConfigValue& value) const {
            if (!m_impl) return ConfigValidationResult();
            ConfigValidationResult result; // 当前校验结果收集器
            ValidateRecursive(value, u"$", result);
            return result;
        }

        // 按 schema 默认值递归补齐对象字段，非对象节点保持不变。
        void ConfigSchema::ApplyDefaults(ConfigValue& value) const {
            if (!m_impl) return;
            ApplyDefaultsRecursive(value);
        }

        // 按 JSONPath 风格路径递归校验类型、数组元素、对象字段和未知 key。
        void ConfigSchema::ValidateRecursive(const ConfigValue& value, const String& path,
            ConfigValidationResult& result) const {
            if (!m_impl) return;

            // 成员上下文内的递归 lambda 可访问每个子 schema 的私有规则而不包装公开值。
            auto validateNode = [&](auto&& self, const ConfigSchema& schema,
                const Internal::ConfigNode& node, const String& currentPath) -> void {
                if (!schema.m_impl) return;
                if (!TypeMatches(schema.m_impl->m_type, node)) {
                    result.AddError(String::Format(u"{} expected {}, got {}", currentPath, SchemaTypeName(schema.m_impl->m_type), NodeTypeName(node)));
                    return;
                }

                if (schema.m_impl->m_hasItemSchema && schema.m_impl->m_itemSchema) {
                    if (const auto* array = ConfigValueAccess::Array(node)) {
                        for (size_t i = 0; i < array->size(); ++i) {
                            self(self, *schema.m_impl->m_itemSchema, (*array)[i], String::Format(u"{}[{}]", currentPath, i));
                        }
                    }
                }

                const auto* object = ConfigValueAccess::Object(node); // 对象字段规则的当前存储
                if (!object || schema.m_impl->m_fields.empty()) return;
                for (const auto& field : schema.m_impl->m_fields) {
                    const auto* child = FindSchemaNode(node, field.m_key); // 只借用目标字段
                    String childPath = currentPath == u"$" ? String(u"$.") + field.m_key : currentPath + u"." + field.m_key;
                    if (!child) {
                        if (field.m_required) result.AddError(String::Format(u"{} is required", childPath));
                        continue;
                    }
                    if (field.m_schema) self(self, *field.m_schema, *child, childPath);
                }

                if (!schema.m_impl->m_allowUnknownKeys) {
                    for (const auto& entry : *object) {
                        const String& key = entry.key.AsString(); // 未知字段诊断 key
                        bool known = std::any_of(
                            schema.m_impl->m_fields.begin(), schema.m_impl->m_fields.end(),
                            [&key](const ConfigSchemaImpl::FieldRule& rule) { return rule.m_key == key; }
                        );
                        if (!known) result.AddError(String::Format(u"$.{} is not allowed", key));
                    }
                }
            };

            validateNode(validateNode, *this, ConfigValueAccess::Node(value), path);
        }

        // 递归写入可选字段默认值，写回父对象保证子对象变更可见。
        void ConfigSchema::ApplyDefaultsRecursive(ConfigValue& value) const {
            if (!m_impl) return;

            // 默认值直接写入唯一内部树，避免 Get/Set 先深克隆子树再写回。
            auto applyNode = [&](auto&& self, const ConfigSchema& schema,
                Internal::ConfigNode& node) -> void {
                if (!schema.m_impl) return;
                auto* object = ConfigValueAccess::Object(node); // 只有对象节点接受字段默认值
                if (!object) return;

                for (const auto& field : schema.m_impl->m_fields) {
                    auto* child = FindSchemaNode(node, field.m_key); // 当前字段或 dotted path
                    if (!child && field.m_hasDefault) {
                        object->push_back(Internal::ConfigObjectEntry{ Internal::ConfigText(field.m_key), Internal::CloneNode(ConfigValueAccess::Node(field.m_defaultValue)) });
                        child = &object->back().value;
                    }
                    if (child && field.m_schema) self(self, *field.m_schema, *child);
                }
            };

            applyNode(applyNode, *this, ConfigValueAccess::Node(value));
        }
    }
}
