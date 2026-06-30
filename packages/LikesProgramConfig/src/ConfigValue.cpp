#include <LikesProgram/Config/ConfigurationInternal.hpp>

namespace LikesProgram {
    namespace Config {
        using namespace Internal;

        // 构造 null 配置节点，内部 variant 默认持有 monostate。
        ConfigValue::ConfigValue() : m_impl(new ConfigValueImpl{}) {
        }

        // 构造字符串节点，保留原始文本内容。
        ConfigValue::ConfigValue(const String& raw) : m_impl(new ConfigValueImpl{}) {
            m_impl->m_value = raw;
        }

        // 从 UTF-16 字面量构造字符串节点，空指针按空字符串处理。
        ConfigValue::ConfigValue(const char16_t* raw) : ConfigValue(String(raw ? raw : u"")) {
        }

        // 构造 int64 数值节点。
        ConfigValue::ConfigValue(int64_t value) : m_impl(new ConfigValueImpl{}) {
            m_impl->m_value = value;
        }

        // int 入口统一提升为 int64，避免 variant 中出现多种整数类型。
        ConfigValue::ConfigValue(int value) : ConfigValue(static_cast<int64_t>(value)) {
        }

        // 构造 double 数值节点。
        ConfigValue::ConfigValue(double value) : m_impl(new ConfigValueImpl{}) {
            m_impl->m_value = value;
        }

        // 构造 bool 节点。
        ConfigValue::ConfigValue(bool value) : m_impl(new ConfigValueImpl{}) {
            m_impl->m_value = value;
        }

        // 深拷贝节点存储，数组和对象会复制完整子树。
        ConfigValue::ConfigValue(const ConfigValue& other) : m_impl(new ConfigValueImpl{}) {
            if (other.m_impl) m_impl->m_value = other.m_impl->m_value;
        }

        // 移动接管节点实现对象，源对象置空后仍可析构。
        ConfigValue::ConfigValue(ConfigValue&& other) noexcept : m_impl(other.m_impl) {
            other.m_impl = nullptr;
        }

        // 释放配置节点实现对象。
        ConfigValue::~ConfigValue() {
            delete m_impl;
            m_impl = nullptr;
        }

        // 拷贝赋值节点存储，源为空时回到 null。
        ConfigValue& ConfigValue::operator=(const ConfigValue& other) {
            if (this == &other) return *this;
            EnsureImpl();
            m_impl->m_value = other.m_impl ? other.m_impl->m_value : ConfigStorage{};
            return *this;
        }

        // 移动赋值节点实现，释放目标旧存储。
        ConfigValue& ConfigValue::operator=(ConfigValue&& other) noexcept {
            if (this == &other) return *this;
            delete m_impl;
            m_impl = other.m_impl;
            other.m_impl = nullptr;
            return *this;
        }

        // 创建显式 null 节点。
        ConfigValue ConfigValue::Null() {
            return ConfigValue();
        }

        // 创建空数组节点。
        ConfigValue ConfigValue::Array() {
            ConfigValue value; // 待返回的数组节点
            value.m_impl->m_value = ConfigArray{};
            return value;
        }

        // 创建空对象节点。
        ConfigValue ConfigValue::Object() {
            ConfigValue value; // 待返回的对象节点
            value.m_impl->m_value = ConfigObject{};
            return value;
        }

        // 确保 moved-from 节点后续可重新写入。
        void ConfigValue::EnsureImpl() {
            if (!m_impl) m_impl = new ConfigValueImpl{};
        }

        // 根据 variant 当前分支返回配置节点类型。
        ConfigValueType ConfigValue::Type() const {
            const auto& storage = ConfigValueAccess::Storage(*this); // 当前节点只读存储
            if (std::holds_alternative<std::monostate>(storage)) return ConfigValueType::Null;
            if (std::holds_alternative<String>(storage)) return ConfigValueType::String;
            if (std::holds_alternative<int64_t>(storage)) return ConfigValueType::Int64;
            if (std::holds_alternative<double>(storage)) return ConfigValueType::Double;
            if (std::holds_alternative<bool>(storage)) return ConfigValueType::Bool;
            if (std::holds_alternative<ConfigArray>(storage)) return ConfigValueType::Array;
            if (std::holds_alternative<ConfigObject>(storage)) return ConfigValueType::Object;
            return ConfigValueType::Null;
        }

        // 返回类型名称，供错误消息、诊断和调试输出使用。
        String ConfigValue::TypeName() const {
            switch (Type()) {
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

        // 判断节点是否为 null。
        bool ConfigValue::IsNull() const { return Type() == ConfigValueType::Null; }
        // 判断节点是否为字符串。
        bool ConfigValue::IsString() const { return Type() == ConfigValueType::String; }
        // 判断节点是否为 int64。
        bool ConfigValue::IsInt64() const { return Type() == ConfigValueType::Int64; }
        // 判断节点是否为 double。
        bool ConfigValue::IsDouble() const { return Type() == ConfigValueType::Double; }
        // 判断节点是否为 bool。
        bool ConfigValue::IsBool() const { return Type() == ConfigValueType::Bool; }
        // 判断节点是否为数组。
        bool ConfigValue::IsArray() const { return Type() == ConfigValueType::Array; }
        // 判断节点是否为对象。
        bool ConfigValue::IsObject() const { return Type() == ConfigValueType::Object; }
        // 判断节点是否为任一数值类型。
        bool ConfigValue::IsNumber() const { return IsInt64() || IsDouble(); }
        // 判断节点是否为非容器标量。
        bool ConfigValue::IsScalar() const { return IsNull() || IsString() || IsNumber() || IsBool(); }

        // 读取原始字符串分支；非字符串或空实现返回稳定空串引用。
        const String& ConfigValue::Raw() const noexcept {
            static const String empty; // 非字符串节点共享的空串哨兵
            if (!m_impl) return empty;
            const auto* raw = std::get_if<String>(&m_impl->m_value); // 字符串分支指针
            return raw ? *raw : empty;
        }

        // 判断节点是否语义为空：null、空字符串、空数组或空对象。
        bool ConfigValue::Empty() const {
            if (IsNull()) return true;
            if (IsString()) return Raw().Empty();
            if (const auto* array = ConfigValueAccess::Array(*this)) return array->empty();
            if (const auto* object = ConfigValueAccess::Object(*this)) return object->empty();
            return false;
        }

        // 将节点转为字符串，复杂容器用紧凑 JSON 表示。
        String ConfigValue::AsString(const String& defaultValue) const {
            if (IsNull()) return defaultValue;
            if (IsString()) return Raw().Empty() ? defaultValue : Raw();
            if (IsInt64()) return String(AsInt64());
            if (IsDouble()) return FormatDouble(AsDouble());
            if (IsBool()) return AsBool() ? u"true" : u"false";
            return ToJson(-1);
        }

        // 将节点转为 int64，字符串走严格解析，失败返回默认值。
        int64_t ConfigValue::AsInt64(int64_t defaultValue) const {
            if (IsInt64()) return std::get<int64_t>(ConfigValueAccess::Storage(*this));
            if (IsBool()) return std::get<bool>(ConfigValueAccess::Storage(*this)) ? 1 : 0;
            if (IsDouble()) return static_cast<int64_t>(std::get<double>(ConfigValueAccess::Storage(*this)));

            int64_t parsed = 0; // 字符串严格解析后的整数值
            return ParseInt64Strict(Raw(), parsed) ? parsed : defaultValue;
        }

        // 将节点转为 double，字符串走严格解析，失败返回默认值。
        double ConfigValue::AsDouble(double defaultValue) const {
            if (IsDouble()) return std::get<double>(ConfigValueAccess::Storage(*this));
            if (IsInt64()) return static_cast<double>(std::get<int64_t>(ConfigValueAccess::Storage(*this)));
            if (IsBool()) return std::get<bool>(ConfigValueAccess::Storage(*this)) ? 1.0 : 0.0;

            double parsed = 0.0; // 字符串严格解析后的浮点值
            return ParseDoubleStrict(Raw(), parsed) ? parsed : defaultValue;
        }

        // 将节点转为 bool，字符串支持常见开关同义词。
        bool ConfigValue::AsBool(bool defaultValue) const {
            if (IsBool()) return std::get<bool>(ConfigValueAccess::Storage(*this));
            if (IsInt64()) return std::get<int64_t>(ConfigValueAccess::Storage(*this)) != 0;
            if (IsDouble()) return std::get<double>(ConfigValueAccess::Storage(*this)) != 0.0;

            bool parsed = false; // 字符串严格解析后的布尔值
            return ParseBoolStrict(Raw(), parsed) ? parsed : defaultValue;
        }

        // 返回数组长度或对象字段数，标量节点返回 0。
        size_t ConfigValue::Size() const {
            if (const auto* array = ConfigValueAccess::Array(*this)) return array->size();
            if (const auto* object = ConfigValueAccess::Object(*this)) return object->size();
            return 0;
        }

        // 判断对象节点是否包含 key，支持 FindObjectEntry 的线性顺序查找。
        bool ConfigValue::Contains(const String& key) const {
            const auto* object = ConfigValueAccess::Object(*this); // 当前对象分支
            return object && FindObjectEntry(*object, key);
        }

        // 读取对象字段，直接 key 优先，未命中时支持 dotted path 递归访问。
        ConfigValue ConfigValue::Get(const String& key) const {
            const auto* object = ConfigValueAccess::Object(*this); // 当前对象分支
            if (!object) return ConfigValue();

            if (const auto* entry = FindObjectEntry(*object, key)) return entry->value;
            if (!HasDot(key)) return ConfigValue();

            const ConfigValue* current = this; // dotted path 遍历只借用节点，避免逐层复制子树
            for (const auto& part : SplitDottedPath(key)) {
                const auto* currentObject = ConfigValueAccess::Object(*current); // 当前层对象分支
                if (!currentObject) return ConfigValue();

                const auto* entry = FindObjectEntry(*currentObject, part); // 当前 path 片段对应字段
                if (!entry) return ConfigValue();
                current = &entry->value;
            }
            return *current;
        }

        // 读取数组元素，越界或非数组时返回 null 节点。
        ConfigValue ConfigValue::At(size_t index) const {
            const auto* array = ConfigValueAccess::Array(*this); // 当前数组分支
            if (!array || index >= array->size()) return ConfigValue();
            return (*array)[index];
        }

        // 写入对象字段，非对象节点会被自动替换为空对象。
        void ConfigValue::Set(const String& key, const ConfigValue& value) {
            EnsureImpl();
            String normalizedKey = TrimAscii(key); // 裁剪后的字段名或 dotted path
            if (normalizedKey.Empty()) return;

            if (!IsObject()) m_impl->m_value = ConfigObject{};
            auto& object = std::get<ConfigObject>(m_impl->m_value); // 可写对象分支

            if (auto* entry = FindObjectEntry(object, normalizedKey)) {
                entry->value = value;
                return;
            }

            object.push_back(ConfigObjectEntry{ normalizedKey, value });
        }

        // 移动写入对象字段，适合解析器把临时子树挂到父对象。
        void ConfigValue::Set(const String& key, ConfigValue&& value) {
            // 移动设置仍复用同一套 key 归一化和对象自动转换语义。
            EnsureImpl();
            String normalizedKey = TrimAscii(key); // 裁剪后的字段名或 dotted path
            if (normalizedKey.Empty()) return;

            if (!IsObject()) m_impl->m_value = ConfigObject{};
            auto& object = std::get<ConfigObject>(m_impl->m_value); // 可写对象分支

            // 重复 key 保持覆盖语义，移动赋值避免复制大数组/对象。
            if (auto* entry = FindObjectEntry(object, normalizedKey)) {
                entry->value = std::move(value);
                return;
            }

            object.push_back(ConfigObjectEntry{ normalizedKey, std::move(value) });
        }

        // 向数组追加节点，非数组节点会被自动替换为空数组。
        void ConfigValue::PushBack(const ConfigValue& value) {
            EnsureImpl();
            if (!IsArray()) m_impl->m_value = ConfigArray{};
            std::get<ConfigArray>(m_impl->m_value).push_back(value);
        }

        // 向数组移动追加节点，减少解析大数组时的子树复制。
        void ConfigValue::PushBack(ConfigValue&& value) {
            // 数组热路径接收临时值时直接转移 PImpl，减少解析阶段深拷贝。
            EnsureImpl();
            if (!IsArray()) m_impl->m_value = ConfigArray{};
            std::get<ConfigArray>(m_impl->m_value).push_back(std::move(value));
        }

        // 从对象中移除 key，返回是否实际删除了字段。
        bool ConfigValue::Remove(const String& key) {
            auto* object = ConfigValueAccess::Object(*this); // 可写对象分支
            if (!object) return false;

            String normalizedKey = TrimAscii(key); // 裁剪后的目标字段名
            auto oldSize = object->size(); // 删除前字段数量
            object->erase(std::remove_if(object->begin(), object->end(),
                [&normalizedKey](const ConfigObjectEntry& entry) {
                    return entry.key == normalizedKey;
                }), object->end());

            return object->size() != oldSize;
        }

        // 返回对象字段名列表，保持对象内部插入顺序。
        std::vector<String> ConfigValue::Keys() const {
            std::vector<String> keys; // 输出字段名列表
            if (const auto* object = ConfigValueAccess::Object(*this)) {
                keys.reserve(object->size());
                for (const auto& entry : *object) keys.push_back(entry.key);
            }
            return keys;
        }

        // 比较两个节点的完整存储内容，数组和对象按顺序比较。
        bool ConfigValue::operator==(const ConfigValue& other) const {
            return ConfigValueAccess::Storage(*this) == ConfigValueAccess::Storage(other);
        }
    }
}
