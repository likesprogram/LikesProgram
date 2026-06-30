#include <LikesProgram/Config/ConfigurationInternal.hpp>

namespace LikesProgram {
    namespace Config {
        using namespace Internal;

        struct Configuration::ConfigurationImpl {
            ConfigValue m_root; // 配置根对象，默认保持 object 语义
        };

        // 创建空对象根配置，便于后续 Set 直接写入字段。
        Configuration::Configuration() : m_impl(new ConfigurationImpl{}) {
            m_impl->m_root = ConfigValue::Object();
        }

        // 从已有根节点构造配置，按值复制保持调用方对象不变。
        Configuration::Configuration(const ConfigValue& root) : m_impl(new ConfigurationImpl{}) {
            m_impl->m_root = root;
        }

        // 从临时根节点构造配置，避免解析结果树重复深拷贝。
        Configuration::Configuration(ConfigValue&& root) : m_impl(new ConfigurationImpl{}) {
            // 解析器返回完整值树时直接接管，避免再深拷贝数组/对象。
            m_impl->m_root = std::move(root);
        }

        // 深拷贝配置根，复制后两个 Configuration 可独立修改。
        Configuration::Configuration(const Configuration& other)
            : m_impl(new ConfigurationImpl{}) {
            if (other.m_impl) m_impl->m_root = other.m_impl->m_root;
        }

        // 移动接管实现对象，源对象置空后仍可安全析构。
        Configuration::Configuration(Configuration&& other) noexcept
            : m_impl(other.m_impl) {
            other.m_impl = nullptr;
        }

        // 释放配置实现对象。
        Configuration::~Configuration() {
            delete m_impl;
            m_impl = nullptr;
        }

        // 拷贝赋值根节点，源对象为空时回到空对象配置。
        Configuration& Configuration::operator=(const Configuration& other) {
            if (this == &other) return *this;
            EnsureImpl();
            m_impl->m_root = other.m_impl ? other.m_impl->m_root : ConfigValue::Object();
            return *this;
        }

        // 移动赋值直接转移 PImpl，并释放目标旧实现。
        Configuration& Configuration::operator=(Configuration&& other) noexcept {
            if (this == &other) return *this;
            delete m_impl;
            m_impl = other.m_impl;
            other.m_impl = nullptr;
            return *this;
        }

        // 确保 moved-from 对象重新获得空对象根。
        void Configuration::EnsureImpl() {
            if (!m_impl) {
                m_impl = new ConfigurationImpl{};
                m_impl->m_root = ConfigValue::Object();
            }
        }

        // 返回配置根节点副本，空实现时返回空对象。
        ConfigValue Configuration::Root() const {
            if (!m_impl) return ConfigValue::Object();
            return m_impl->m_root;
        }

        // 复制设置根节点，不要求根节点必须是 object。
        void Configuration::SetRoot(const ConfigValue& value) {
            EnsureImpl();
            m_impl->m_root = value;
        }

        // 移动设置根节点，供解析器或构建器交出大对象树。
        void Configuration::SetRoot(ConfigValue&& value) {
            // 构建器或解析器交出根节点时直接转移 PImpl 所有权。
            EnsureImpl();
            m_impl->m_root = std::move(value);
        }

        // 以字符串值写入 key，key 会先做 ASCII 空白裁剪。
        void Configuration::Set(const String& key, const String& value) {
            SetValue(key, ConfigValue(value));
        }

        // 写入任意 ConfigValue，必要时根节点由 ConfigValue::Set 转为 object。
        void Configuration::SetValue(const String& key, const ConfigValue& value) {
            EnsureImpl();
            String normalizedKey = TrimAscii(key); // 归一化后的字段路径
            if (normalizedKey.Empty()) return;
            m_impl->m_root.Set(normalizedKey, value);
        }

        // 判断根对象或 dotted path 中是否存在指定 key。
        bool Configuration::Contains(const String& key) const {
            return m_impl && m_impl->m_root.Contains(TrimAscii(key));
        }

        // 获取指定 key 的节点，缺失时返回 null ConfigValue。
        ConfigValue Configuration::Get(const String& key) const {
            if (!m_impl) return ConfigValue();
            return m_impl->m_root.Get(TrimAscii(key));
        }

        // 获取字符串值，缺失或无法转为有效字符串时使用默认值。
        String Configuration::GetString(const String& key, const String& defaultValue) const {
            return Get(key).AsString(defaultValue);
        }

        // 获取 int64 值，支持字符串严格解析和 bool/浮点转换。
        int64_t Configuration::GetInt64(const String& key, int64_t defaultValue) const {
            return Get(key).AsInt64(defaultValue);
        }

        // 获取 double 值，支持字符串严格解析和整数/bool 转换。
        double Configuration::GetDouble(const String& key, double defaultValue) const {
            return Get(key).AsDouble(defaultValue);
        }

        // 获取 bool 值，支持常见字符串同义词和数值转换。
        bool Configuration::GetBool(const String& key, bool defaultValue) const {
            return Get(key).AsBool(defaultValue);
        }

        // 从根对象移除指定 key，空实现或非对象时返回 false。
        bool Configuration::Remove(const String& key) {
            if (!m_impl) return false;
            return m_impl->m_root.Remove(TrimAscii(key));
        }

        // 重置为空对象根，保留 Configuration 对象可继续复用。
        void Configuration::Clear() {
            EnsureImpl();
            m_impl->m_root = ConfigValue::Object();
        }

        // 返回根数组或根对象的元素数量，标量根返回 0。
        size_t Configuration::Size() const {
            return m_impl ? m_impl->m_root.Size() : 0;
        }

        // 解析简单 key=value 文本，忽略空行、注释行和无等号行。
        Configuration Configuration::FromKeyValueLines(const String& text) {
            Configuration configuration; // 解析输出配置
            for (const auto& line : SplitLines(text)) {
                String trimmed = TrimAscii(line); // 当前行去首尾空白后的内容
                if (trimmed.Empty() || trimmed.StartsWith(u"#")) continue;

                size_t equals = FindEquals(trimmed); // key/value 分隔等号位置
                if (equals == String::npos) continue;

                String key = TrimAscii(trimmed.SubString(0, equals)); // 裁剪后的 key
                String value = TrimAscii(trimmed.SubString(equals + 1, trimmed.Size() - equals - 1)); // 裁剪后的 value
                configuration.Set(key, value);
            }

            return configuration;
        }

        namespace {
            // 将对象树展平为 dotted key=value 行，嵌套对象递归展开。
            void AppendKeyValueLines(const ConfigValue& value, const String& prefix, String& output) {
                if (!value.IsObject()) return;

                ForEachObjectEntry(value, [&](const String& name, const ConfigValue& child) {
                    String key = prefix.Empty() ? name : prefix + u"." + name; // 当前节点完整 dotted key
                    if (child.IsObject()) {
                        AppendKeyValueLines(child, key, output);
                    }
                    else {
                        output.Append(key);
                        output.Append(u'=');
                        output.Append(child.IsString() ? child.AsString() : child.ToJson(-1));
                        output.Append(u'\n');
                    }
                });
            }
        }

        // 将当前配置导出为简单 key=value 行，复杂标量用 JSON 表示。
        String Configuration::ToKeyValueLines() const {
            String text; // 输出文本缓冲
            if (!m_impl) return text;
            AppendKeyValueLines(m_impl->m_root, String(), text);
            return text;
        }

        // 尝试从 JSON 文本解析配置，错误通过 Result 状态返回。
        Result<Configuration> Configuration::TryFromJson(const String& text) {
            auto result = ConfigValue::TryParseJson(text); // JSON 根节点解析结果
            if (!result.IsOk()) return result.GetStatus();
            return Configuration(result.MoveValue());
        }

        // 从 JSON 文本解析配置，失败时抛出 std::runtime_error。
        Configuration Configuration::FromJson(const String& text) {
            auto result = TryFromJson(text); // 解析结果，失败时转异常接口
            if (!result.IsOk()) throw std::runtime_error(result.GetStatus().ToString().ToStdString());
            return result.MoveValue();
        }

        // 将配置根节点序列化为 JSON，空实现输出空对象。
        String Configuration::ToJson(int indent) const {
            if (!m_impl) return ConfigValue::Object().ToJson(indent);
            return m_impl->m_root.ToJson(indent);
        }

        // 尝试从 YAML 文本解析配置，错误通过 Result 状态返回。
        Result<Configuration> Configuration::TryFromYaml(const String& text) {
            auto result = ConfigValue::TryParseYaml(text); // YAML 根节点解析结果
            if (!result.IsOk()) return result.GetStatus();
            return Configuration(result.MoveValue());
        }

        // 从 YAML 文本解析配置，失败时抛出 std::runtime_error。
        Configuration Configuration::FromYaml(const String& text) {
            auto result = TryFromYaml(text); // 解析结果，失败时转异常接口
            if (!result.IsOk()) throw std::runtime_error(result.GetStatus().ToString().ToStdString());
            return result.MoveValue();
        }

        // 将配置根节点序列化为 YAML，空实现输出空对象。
        String Configuration::ToYaml(int indent) const {
            if (!m_impl) return ConfigValue::Object().ToYaml(indent);
            return m_impl->m_root.ToYaml(indent);
        }

        // 尝试从 TOML 文本解析配置，错误通过 Result 状态返回。
        Result<Configuration> Configuration::TryFromToml(const String& text) {
            auto result = ConfigValue::TryParseToml(text); // TOML 根节点解析结果
            if (!result.IsOk()) return result.GetStatus();
            return Configuration(result.MoveValue());
        }

        // 从 TOML 文本解析配置，失败时抛出 std::runtime_error。
        Configuration Configuration::FromToml(const String& text) {
            auto result = TryFromToml(text); // 解析结果，失败时转异常接口
            if (!result.IsOk()) throw std::runtime_error(result.GetStatus().ToString().ToStdString());
            return result.MoveValue();
        }

        // 将配置根节点序列化为 TOML，空实现输出空对象。
        String Configuration::ToToml() const {
            if (!m_impl) return ConfigValue::Object().ToToml();
            return m_impl->m_root.ToToml();
        }

        // 使用 schema 校验当前根节点，返回完整错误集合。
        ConfigValidationResult Configuration::Validate(const ConfigSchema& schema) const {
            return schema.Validate(Root());
        }

        // 按 schema 默认值补齐当前配置根节点。
        void Configuration::ApplyDefaults(const ConfigSchema& schema) {
            EnsureImpl();
            schema.ApplyDefaults(m_impl->m_root);
        }
    }
}
