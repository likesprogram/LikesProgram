#include "LikesProgram/Config/ConfigurationInternal.hpp"

namespace LikesProgram {
    namespace Config {
        using namespace Internal;

        namespace {
            // Configuration 的便捷 API 把 dotted key 解释为对象路径；ConfigValue 仍保留直接 key 语义。
            ConfigNode* EnsurePathParent(ConfigValue& root, const std::vector<String>& parts) {
                if (!root.IsObject()) root = ConfigValue::Object();

                ConfigNode* current = &ConfigValueAccess::Node(root); // 当前内部路径节点
                for (size_t i = 0; i + 1 < parts.size(); ++i) {
                    auto* object = ConfigValueAccess::Object(*current);
                    auto* entry = FindObjectEntry(*object, parts[i]);
                    if (!entry) {
                        object->push_back(ConfigObjectEntry{ ConfigText(parts[i]), ConfigNode(ConfigObject{}) });
                        entry = &object->back();
                    } else if (!NodeIs(entry->value, ConfigValueType::Object)) {
                        entry->value = ConfigNode(ConfigObject{});
                    }
                    current = &entry->value;
                }
                return current;
            }

            // 在内部对象节点写入深克隆字段，保持 Configuration 的 const 值语义。
            void SetConfigurationNode(ConfigNode& node, const String& key, const ConfigValue& value) {
                auto* object = ConfigValueAccess::Object(node); // EnsurePathParent 已保证对象分支
                if (auto* entry = FindObjectEntry(*object, key)) {
                    entry->value = CloneNode(ConfigValueAccess::Node(value));
                    return;
                }
                object->push_back(ConfigObjectEntry{ ConfigText(key), CloneNode(ConfigValueAccess::Node(value)) });
            }

            // 解析器移动写入内部字段，避免为每个 key=value 标量建立公开 PImpl。
            void SetConfigurationNode(ConfigNode& node, const String& key, ConfigNode&& value) {
                auto* object = ConfigValueAccess::Object(node); // EnsurePathParent 已保证对象分支
                if (auto* entry = FindObjectEntry(*object, key)) {
                    entry->value = std::move(value);
                    return;
                }
                object->push_back(ConfigObjectEntry{ ConfigText(key), std::move(value) });
            }

            // 从内部对象节点移除直接字段，不把末端 key 再解释为 dotted path。
            bool RemoveConfigurationNode(ConfigNode& node, const String& key) {
                auto* object = ConfigValueAccess::Object(node); // 当前末端父对象
                if (!object) return false;

                const auto oldSize = object->size(); // 删除前字段数量
                object->erase(
                    std::remove_if(object->begin(), object->end(),
                    [&key](const ConfigObjectEntry& entry) { return entry.key.Equals(key); }),
                    object->end()
                );
                return object->size() != oldSize;
            }

            // 写入路径时覆盖末端字段，保留同级字段和插入顺序。
            void SetConfigurationPath(ConfigValue& root, const String& key, const ConfigValue& value) {
                const auto parts = SplitDottedPath(key);
                if (parts.empty()) return;
                if (parts.size() == 1) {
                    root.Set(parts.front(), value);
                    return;
                }

                SetConfigurationNode(*EnsurePathParent(root, parts), parts.back(), value);
            }

            // key=value 解析路径直接接管内部字符串节点。
            void SetConfigurationPath(ConfigValue& root, const String& key, ConfigNode&& value) {
                const auto parts = SplitDottedPath(key); // 当前 dotted key 路径
                if (parts.empty()) return;
                if (parts.size() == 1) {
                    if (!root.IsObject()) root = ConfigValue::Object();
                    SetConfigurationNode(ConfigValueAccess::Node(root), parts.front(), std::move(value));
                    return;
                }

                SetConfigurationNode(*EnsurePathParent(root, parts), parts.back(), std::move(value));
            }

            // 删除路径末端字段；中间节点不存在或不是对象时返回 false。
            bool RemoveConfigurationPath(ConfigValue& root, const String& key) {
                const auto parts = SplitDottedPath(key);
                if (parts.empty()) return false;
                if (parts.size() == 1) return root.Remove(parts.front());

                ConfigNode* current = &ConfigValueAccess::Node(root); // 根内部节点
                for (size_t i = 0; i + 1 < parts.size(); ++i) {
                    auto* object = ConfigValueAccess::Object(*current);
                    if (!object) return false;
                    auto* entry = FindObjectEntry(*object, parts[i]);
                    if (!entry) return false;
                    current = &entry->value;
                }
                return RemoveConfigurationNode(*current, parts.back());
            }
        }

        struct Configuration::ConfigurationImpl {
            ConfigValue m_root; // 配置根对象，默认保持 object 语义
        };

        Configuration::Configuration() : m_impl(new ConfigurationImpl{}) {
            m_impl->m_root = ConfigValue::Object();
        }

        Configuration::Configuration(const ConfigValue& root) : m_impl(new ConfigurationImpl{}) {
            m_impl->m_root = root;
        }

        Configuration::Configuration(ConfigValue&& root) : m_impl(new ConfigurationImpl{}) {
            // 解析器返回完整值树时直接接管，避免再深拷贝数组/对象。
            m_impl->m_root = std::move(root);
        }

        Configuration::Configuration(const Configuration& other) : m_impl(new ConfigurationImpl{}) {
            if (other.m_impl) m_impl->m_root = other.m_impl->m_root;
        }

        Configuration::Configuration(Configuration&& other) noexcept : m_impl(other.m_impl) {
            other.m_impl = nullptr;
        }

        Configuration::~Configuration() {
            delete m_impl;
            m_impl = nullptr;
        }

        Configuration& Configuration::operator=(const Configuration& other) {
            if (this == &other) return *this;
            EnsureImpl();
            m_impl->m_root = other.m_impl ? other.m_impl->m_root : ConfigValue::Object();
            return *this;
        }

        Configuration& Configuration::operator=(Configuration&& other) noexcept {
            if (this == &other) return *this;
            delete m_impl;
            m_impl = other.m_impl;
            other.m_impl = nullptr;
            return *this;
        }

        void Configuration::EnsureImpl() {
            if (!m_impl) {
                m_impl = new ConfigurationImpl{};
                m_impl->m_root = ConfigValue::Object();
            }
        }

        ConfigValue Configuration::Root() const {
            if (!m_impl) return ConfigValue::Object();
            return m_impl->m_root;
        }

        void Configuration::SetRoot(const ConfigValue& value) {
            EnsureImpl();
            m_impl->m_root = value;
        }

        void Configuration::SetRoot(ConfigValue&& value) {
            // 构建器或解析器交出根节点时直接转移 PImpl 所有权。
            EnsureImpl();
            m_impl->m_root = std::move(value);
        }

        void Configuration::Set(const String& key, const String& value) {
            SetValue(key, ConfigValue(value));
        }

        void Configuration::SetValue(const String& key, const ConfigValue& value) {
            EnsureImpl();
            String normalizedKey = TrimAscii(key); // 归一化后的字段路径
            if (normalizedKey.Empty()) return;
            SetConfigurationPath(m_impl->m_root, normalizedKey, value);
        }

        bool Configuration::Contains(const String& key) const {
            return m_impl && m_impl->m_root.Contains(TrimAscii(key));
        }

        ConfigValue Configuration::Get(const String& key) const {
            if (!m_impl) return ConfigValue();
            return m_impl->m_root.Get(TrimAscii(key));
        }

        String Configuration::GetString(const String& key, const String& defaultValue) const {
            return Get(key).AsString(defaultValue);
        }

        int64_t Configuration::GetInt64(const String& key, int64_t defaultValue) const {
            return Get(key).AsInt64(defaultValue);
        }

        double Configuration::GetDouble(const String& key, double defaultValue) const {
            return Get(key).AsDouble(defaultValue);
        }

        bool Configuration::GetBool(const String& key, bool defaultValue) const {
            return Get(key).AsBool(defaultValue);
        }

        bool Configuration::Remove(const String& key) {
            if (!m_impl) return false;
            return RemoveConfigurationPath(m_impl->m_root, TrimAscii(key));
        }

        void Configuration::Clear() {
            EnsureImpl();
            m_impl->m_root = ConfigValue::Object();
        }

        size_t Configuration::Size() const {
            return m_impl ? m_impl->m_root.Size() : 0;
        }

        Configuration Configuration::FromKeyValueLines(const String& text) {
            Configuration configuration; // 解析输出配置
            for (const auto& line : SplitLines(text)) {
                String trimmed = TrimAscii(line); // 当前行去首尾空白后的内容
                if (trimmed.Empty() || trimmed.StartsWith(u"#")) continue;

                size_t equals = FindEquals(trimmed); // key/value 分隔等号位置
                if (equals == String::npos) continue;

                String key = TrimAscii(trimmed.SubString(0, equals)); // 裁剪后的 key
                String value = TrimAscii(trimmed.SubString(equals + 1, trimmed.Size() - equals - 1)); // 裁剪后的 value
                SetConfigurationPath(configuration.m_impl->m_root, key, ConfigNode(ConfigText(std::move(value))));
            }

            return configuration;
        }

        namespace {
            // 将对象树展平为 dotted key=value 行，嵌套对象递归展开。
            void AppendKeyValueLines(const ConfigNode& value, const String& prefix, String& output) {
                if (!NodeIs(value, ConfigValueType::Object)) return;

                ForEachObjectEntry(value, [&](const String& name, const ConfigNode& child) {
                    String key = prefix.Empty() ? name : prefix + u"." + name; // 当前节点完整 dotted key
                    if (NodeIs(child, ConfigValueType::Object)) AppendKeyValueLines(child, key, output);
                    else {
                        output.Append(key);
                        output.Append(u'=');
                        output.Append(NodeIs(child, ConfigValueType::String) ? NodeString(child) : SerializeJsonDocument(child, -1));
                        output.Append(u'\n');
                    }
                });
            }
        }

        String Configuration::ToKeyValueLines() const {
            String text; // 输出文本缓冲
            if (!m_impl) return text;
            AppendKeyValueLines(ConfigValueAccess::Node(m_impl->m_root), String(), text);
            return text;
        }

        Result<Configuration> Configuration::TryFromJson(const String& text) {
            auto result = ConfigValue::TryParseJson(text); // JSON5 根节点解析结果
            if (!result.IsOk()) return result.PropagateFailure<Configuration>();
            return Configuration(result.MoveValue());
        }

        Configuration Configuration::FromJson(const String& text) {
            auto result = TryFromJson(text); // 解析结果，失败时转异常接口
            if (!result.IsOk()) throw std::runtime_error(result.GetStatus().ToString().ToStdString());
            return result.MoveValue();
        }

        String Configuration::ToJson(int indent) const {
            if (!m_impl) return ConfigValue::Object().ToJson(indent);
            return m_impl->m_root.ToJson(indent);
        }

        Result<Configuration> Configuration::TryFromJson5(const String& text) {
            return TryFromJson(text);
        }

        Configuration Configuration::FromJson5(const String& text) {
            return FromJson(text);
        }

        String Configuration::ToJson5(int indent) const {
            return ToJson(indent);
        }

        Result<Configuration> Configuration::TryFromYaml(const String& text) {
            auto result = ConfigValue::TryParseYaml(text); // YAML 根节点解析结果
            if (!result.IsOk()) return result.GetStatus();
            return Configuration(result.MoveValue());
        }

        Configuration Configuration::FromYaml(const String& text) {
            auto result = TryFromYaml(text); // 解析结果，失败时转异常接口
            if (!result.IsOk()) throw std::runtime_error(result.GetStatus().ToString().ToStdString());
            return result.MoveValue();
        }

        String Configuration::ToYaml(int indent) const {
            if (!m_impl) return ConfigValue::Object().ToYaml(indent);
            return m_impl->m_root.ToYaml(indent);
        }

        Result<Configuration> Configuration::TryFromToml(const String& text) {
            auto result = ConfigValue::TryParseToml(text); // TOML 根节点解析结果
            if (!result.IsOk()) return result.GetStatus();
            return Configuration(result.MoveValue());
        }

        Configuration Configuration::FromToml(const String& text) {
            auto result = TryFromToml(text); // 解析结果，失败时转异常接口
            if (!result.IsOk()) throw std::runtime_error(result.GetStatus().ToString().ToStdString());
            return result.MoveValue();
        }

        String Configuration::ToToml() const {
            if (!m_impl) return ConfigValue::Object().ToToml();
            return m_impl->m_root.ToToml();
        }

        ConfigValidationResult Configuration::Validate(const ConfigSchema& schema) const {
            return schema.Validate(Root());
        }

        void Configuration::ApplyDefaults(const ConfigSchema& schema) {
            EnsureImpl();
            schema.ApplyDefaults(m_impl->m_root);
        }
    }
}
