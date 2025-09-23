#include "../../include/LikesProgram/Configuration.hpp"
#include <stdexcept>
#include <mutex>
#include <limits>
#include <sstream>
#include <iomanip>

namespace LikesProgram {
    struct Configuration::ConfigurationImpl {
        Value value_;

        mutable std::shared_mutex mutex_;  // 每个对象独有

        mutable std::shared_mutex g_serializer_mutex; // 序列化器锁

        std::shared_ptr<Serializer> serializer;

        static std::shared_ptr<Serializer> defaultSerializer; // 全局序列化器
        static std::shared_mutex g_default_serializer_mutex; // 全局序列化器锁
    };

    // 静态成员初始化
    std::shared_ptr<Configuration::Serializer> Configuration::ConfigurationImpl::defaultSerializer = CreateJsonSerializer();
    std::shared_mutex Configuration::ConfigurationImpl::g_default_serializer_mutex;

    Configuration::Configuration(): m_impl(new ConfigurationImpl{}) {
    }
    Configuration::Configuration(int v): m_impl(new ConfigurationImpl{}) {
        m_impl->value_ = v;
    }
    Configuration::Configuration(int64_t v): m_impl(new ConfigurationImpl{}) {
        m_impl->value_ = v;
    }
    Configuration::Configuration(double v): m_impl(new ConfigurationImpl{}) {
        m_impl->value_ = v;
    }
    Configuration::Configuration(bool v): m_impl(new ConfigurationImpl{}) {
        m_impl->value_ = v;
    }
    Configuration::Configuration(const char* v): m_impl(new ConfigurationImpl{}) {
        m_impl->value_ = String(v);
    }
    Configuration::Configuration(const char8_t* v): m_impl(new ConfigurationImpl{}) {
        m_impl->value_ = String(v);
    }
    Configuration::Configuration(const char16_t* v): m_impl(new ConfigurationImpl{}) {
        m_impl->value_ = String(v);
    }
    Configuration::Configuration(const char32_t* v): m_impl(new ConfigurationImpl{}) {
        m_impl->value_ = String(v);
    }
    Configuration::Configuration(const char8_t c): m_impl(new ConfigurationImpl{}) {
        m_impl->value_ = String(c);
    }
    Configuration::Configuration(const char16_t c): m_impl(new ConfigurationImpl{}) {
        m_impl->value_ = String(c);
    }
    Configuration::Configuration(const char32_t c): m_impl(new ConfigurationImpl{}) {
        m_impl->value_ = String(c);
    }
    Configuration::Configuration(String v): m_impl(new ConfigurationImpl{}) {
        m_impl->value_ = std::move(v);
    }
    Configuration::Configuration(Array v): m_impl(new ConfigurationImpl{}) {
        m_impl->value_ = std::move(v);
    }
    Configuration::Configuration(ObjectMap v): m_impl(new ConfigurationImpl{}) {
        m_impl->value_ = std::move(v);
    }
    Configuration::Configuration(const Configuration& other): m_impl(new ConfigurationImpl{}) {
        // 先拷贝 value_
        {
            std::shared_lock lock(other.m_impl->mutex_); // 共享锁保护 value_
            m_impl->value_ = other.m_impl->value_;
        }

        // 拷贝 serializer
        {
            std::shared_lock lock(other.m_impl->g_serializer_mutex);
            m_impl->serializer = other.m_impl->serializer;
        }
    }
    Configuration::Configuration(Configuration&& other) noexcept : m_impl(nullptr)
    {
        // 抢占其他对象的 impl
        std::shared_lock lock(other.m_impl->g_serializer_mutex);
        m_impl = other.m_impl;
        other.m_impl = nullptr; // 移交完成
    }

    Configuration::~Configuration() {
        if (m_impl) delete m_impl;
        m_impl = nullptr;
    }

    Configuration& Configuration::operator=(int v) {
        std::unique_lock lock(m_impl->mutex_);
        m_impl->value_ = v;
        return *this;
    }
    Configuration& Configuration::operator=(int64_t v) {
        std::unique_lock lock(m_impl->mutex_);
        m_impl->value_ = v;
        return *this;
    }
    Configuration& Configuration::operator=(double v) {
        std::unique_lock lock(m_impl->mutex_);
        m_impl->value_ = v;
        return *this;
    }
    Configuration& Configuration::operator=(bool v) {
        std::unique_lock lock(m_impl->mutex_);
        m_impl->value_ = v;
        return *this;
    }
    Configuration& Configuration::operator=(const String& v) {
        std::unique_lock lock(m_impl->mutex_);
        m_impl->value_ = v;
        return *this;
    }
    Configuration& Configuration::operator=(const char* v) {
        std::unique_lock lock(m_impl->mutex_);
        m_impl->value_ = String(v);
        return *this;
    }
    Configuration& Configuration::operator=(const char8_t* v) {
        std::unique_lock lock(m_impl->mutex_);
        m_impl->value_ = String(v);
        return *this;
    }
    Configuration& Configuration::operator=(const char16_t* v) {
        std::unique_lock lock(m_impl->mutex_);
        m_impl->value_ = String(v);
        return *this;
    }
    Configuration& Configuration::operator=(const char32_t* v) {
        std::unique_lock lock(m_impl->mutex_);
        m_impl->value_ = String(v);
        return *this;
    }
    Configuration& Configuration::operator=(const char8_t c) {
        std::unique_lock lock(m_impl->mutex_);
        m_impl->value_ = String(c);
        return *this;
    }
    Configuration& Configuration::operator=(const char16_t c) {
        std::unique_lock lock(m_impl->mutex_);
        m_impl->value_ = String(c);
        return *this;
    }
    Configuration& Configuration::operator=(const char32_t c) {
        std::unique_lock lock(m_impl->mutex_);
        m_impl->value_ = String(c);
        return *this;
    }
    Configuration& Configuration::operator=(Array v) {
        std::unique_lock lock(m_impl->mutex_);
        m_impl->value_ = std::move(v);
        return *this;
    }
    Configuration& Configuration::operator=(ObjectMap v) {
        std::unique_lock lock(m_impl->mutex_);
        m_impl->value_ = std::move(v);
        return *this;
    }
    Configuration& Configuration::operator=(const Configuration& other) {
        if (this == &other) return *this;

        ConfigurationImpl* newImpl = new ConfigurationImpl{};

        // 拷贝 value_
        {
            std::shared_lock lock(other.m_impl->mutex_);
            newImpl->value_ = other.m_impl->value_;
        }

        // 拷贝 serializer
        {
            std::shared_lock lock(other.m_impl->g_serializer_mutex);
            newImpl->serializer = other.m_impl->serializer;
        }

        // 替换当前 impl
        delete m_impl;
        m_impl = newImpl;

        return *this;
    }
    Configuration& Configuration::operator=(Configuration&& other) noexcept {
        if (this == &other) return *this;

        // 删除自己已有的 impl
        delete m_impl;

        // 抢占其他对象的 impl
        {
            std::shared_lock lock(other.m_impl->g_serializer_mutex);
            m_impl = other.m_impl;
            other.m_impl = nullptr;
        }

        return *this;
    }

    bool Configuration::IsNull() const {
        return std::holds_alternative<std::monostate>(m_impl->value_);
    }
    bool Configuration::IsInt() const {
        return std::holds_alternative<int>(m_impl->value_);
    }
    bool Configuration::IsInt64()  const {
        return std::holds_alternative<int64_t>(m_impl->value_);
    }
    bool Configuration::IsDouble() const {
        return std::holds_alternative<double>(m_impl->value_);
    }
    bool Configuration::IsBool()   const {
        return std::holds_alternative<bool>(m_impl->value_);
    }
    bool Configuration::IsString() const {
        return std::holds_alternative<String>(m_impl->value_);
    }
    bool Configuration::IsArray()  const {
        return std::holds_alternative<Array>(m_impl->value_);
    }
    bool Configuration::IsObject() const {
        return std::holds_alternative<ObjectMap>(m_impl->value_);
    }
    bool Configuration::IsNumber() const {
        return IsInt() || IsInt64() || IsDouble();
    }

    String Configuration::TypeName() const {
        std::shared_lock lock(m_impl->mutex_); // 共享锁
        if (IsNull()) return u"null";
        if (IsInt()) return u"int";
        if (IsInt64()) return u"int64";
        if (IsDouble()) return u"double";
        if (IsBool()) return u"bool";
        if (IsString()) return u"string";
        if (IsArray()) return u"array";
        if (IsObject()) return u"object";
        return u"unknown";
    }

    Configuration& Configuration::operator[](const String& key) {
        std::unique_lock lock(m_impl->mutex_); // 独占锁
        EnsureObject();
        return std::get<ObjectMap>(m_impl->value_)[key];
    }

    const Configuration& Configuration::operator[](const String& key) const {
        std::shared_lock lock(m_impl->mutex_); // 共享锁
        if (!IsObject()) throw std::runtime_error("Not an object: cannot access key '" + key.ToStdString() + "'");
        auto& obj = std::get<ObjectMap>(m_impl->value_);
        auto it = obj.find(key);
        if (it == obj.end()) throw std::runtime_error("Key '" + key.ToStdString() + "' not found");
        return it->second;
    }

    Configuration& Configuration::operator[](size_t idx) {
        std::unique_lock lock(m_impl->mutex_); // 独占锁
        EnsureArray();
        auto& arr = std::get<Array>(m_impl->value_);
        if (idx >= arr.size()) throw std::out_of_range("Array index out of range");
        return arr[idx];
    }

    const Configuration& Configuration::operator[](size_t idx) const {
        std::shared_lock lock(m_impl->mutex_); // 共享锁
        if (!IsArray()) throw std::runtime_error("Not an array: cannot access index " + std::to_string(idx));
        auto& arr = std::get<Array>(m_impl->value_);
        if (idx >= arr.size()) throw std::out_of_range("Array index out of range");
        return arr[idx];
    }

    Configuration& Configuration::At(size_t idx) {
        std::unique_lock lock(m_impl->mutex_); // 独占锁
        EnsureArray();
        auto& arr = std::get<Array>(m_impl->value_);
        if (idx >= arr.size()) throw std::out_of_range("Array index out of range");
        return arr[idx];
    }

    const Configuration& Configuration::At(size_t idx) const {
        std::shared_lock lock(m_impl->mutex_); // 共享锁
        if (!IsArray()) throw std::runtime_error("Not an array: cannot access index " + std::to_string(idx));
        auto& arr = std::get<Array>(m_impl->value_);
        if (idx >= arr.size()) throw std::out_of_range("Array index out of range");
        return arr[idx];
    }

    size_t Configuration::Size() const {
        std::shared_lock lock(m_impl->mutex_); // 共享锁
        if (IsArray())  return std::get<Array>(m_impl->value_).size();
        if (IsObject()) return std::get<ObjectMap>(m_impl->value_).size();
        return 0;
    }

    Configuration::Array::iterator Configuration::begin() {
        EnsureArray();
        return std::get<Array>(m_impl->value_).begin();
    }
    Configuration::Array::iterator Configuration::end() {
        EnsureArray(); return std::get<Array>(m_impl->value_).end();
    }
    Configuration::Array::const_iterator Configuration::begin() const {
        return std::get<Array>(m_impl->value_).begin();
    }
    Configuration::Array::const_iterator Configuration::end() const {
        return std::get<Array>(m_impl->value_).end();
    }

    Configuration::ObjectMap::iterator Configuration::beginObject() {
        EnsureObject(); return std::get<ObjectMap>(m_impl->value_).begin();
    }
    Configuration::ObjectMap::iterator Configuration::endObject() {
        EnsureObject(); return std::get<ObjectMap>(m_impl->value_).end();
    }
    Configuration::ObjectMap::const_iterator Configuration::beginObject() const {
        return std::get<ObjectMap>(m_impl->value_).begin();
    }
    Configuration::ObjectMap::const_iterator Configuration::endObject() const {
        return std::get<ObjectMap>(m_impl->value_).end();
    }

    Configuration::ObjectRange Configuration::objects() {
        EnsureObject();
        return ObjectRange(std::get<ObjectMap>(m_impl->value_).begin(), std::get<ObjectMap>(m_impl->value_).end());
    }
    Configuration::ConstObjectRange Configuration::objects() const {
        return ConstObjectRange(std::get<ObjectMap>(m_impl->value_).begin(), std::get<ObjectMap>(m_impl->value_).end());
    }

    int Configuration::AsInt(CastPolicy p) const {
        std::shared_lock lock(m_impl->mutex_); // 共享锁
        try {
            if (IsInt()) return std::get<int>(m_impl->value_);
            if (IsInt64()) {
                int64_t v = std::get<int64_t>(m_impl->value_);
                if (v > std::numeric_limits<int>::max() || v < std::numeric_limits<int>::min())
                    throw std::overflow_error("int64 value out of int range");
                return static_cast<int>(v);
            }
            if (p == CastPolicy::AutoConvert) {
                if (IsDouble()) return static_cast<int>(std::get<double>(m_impl->value_));
                if (IsBool()) return std::get<bool>(m_impl->value_) ? 1 : 0;
                if (IsString()) return std::stoi(std::get<String>(m_impl->value_).ToStdString());
            }
        }
        catch (std::exception& e) {
            throw std::runtime_error("Cannot cast " + TypeName().ToStdString() + " to int: " + e.what());
        }
        throw std::runtime_error("Cannot cast " + TypeName().ToStdString() + " to int");
    }

    int64_t Configuration::AsInt64(CastPolicy p) const {
        std::shared_lock lock(m_impl->mutex_); // 共享锁
        try {
            if (IsInt64()) return std::get<int64_t>(m_impl->value_);
            if (IsInt()) return static_cast<int64_t>(std::get<int>(m_impl->value_));
            if (p == CastPolicy::AutoConvert) {
                if (IsDouble()) return static_cast<int64_t>(std::get<double>(m_impl->value_));
                if (IsBool()) return std::get<bool>(m_impl->value_) ? 1 : 0;
                if (IsString()) return std::stoll(std::get<String>(m_impl->value_).ToStdString());
            }
        }
        catch (std::exception& e) {
            throw std::runtime_error("Cannot cast " + TypeName().ToStdString() + " to int64: " + e.what());
        }
        throw std::runtime_error("Cannot cast " + TypeName().ToStdString() + " to int64");
    }

    double Configuration::AsDouble(CastPolicy p) const {
        std::shared_lock lock(m_impl->mutex_); // 共享锁
        try {
            if (IsDouble()) return std::get<double>(m_impl->value_);
            if (IsInt()) return static_cast<double>(std::get<int>(m_impl->value_));
            if (IsInt64()) return static_cast<double>(std::get<int64_t>(m_impl->value_));
            if (p == CastPolicy::AutoConvert) {
                if (IsBool()) return std::get<bool>(m_impl->value_) ? 1.0 : 0.0;
                if (IsString()) return std::stod(std::get<String>(m_impl->value_).ToStdString());
            }
        }
        catch (std::exception& e) {
            throw std::runtime_error("Cannot cast " + TypeName().ToStdString() + " to double: " + e.what());
        }
        throw std::runtime_error("Cannot cast " + TypeName().ToStdString() + " to double");
    }

    bool Configuration::AsBool(CastPolicy p) const {
        std::shared_lock lock(m_impl->mutex_); // 共享锁
        if (IsBool()) return std::get<bool>(m_impl->value_);
        if (p == CastPolicy::AutoConvert) {
            if (IsInt()) return std::get<int>(m_impl->value_) != 0;
            if (IsInt64()) return std::get<int64_t>(m_impl->value_) != 0;
            if (IsDouble()) return std::get<double>(m_impl->value_) != 0.0;
            if (IsString()) {
                auto s = std::get<String>(m_impl->value_);
                return s == u"true" || s == u"1";
            }
        }
        throw std::runtime_error("Cannot cast " + TypeName().ToStdString() + " to bool");
    }

    String Configuration::AsString(CastPolicy p) const {
        std::shared_lock lock(m_impl->mutex_); // 共享锁
        if (IsString()) return std::get<String>(m_impl->value_);
        if (p == CastPolicy::AutoConvert) {
            if (IsInt()) return String(std::to_string(std::get<int>(m_impl->value_)));
            if (IsInt64()) return String(std::to_string(std::get<int64_t>(m_impl->value_)));
            if (IsDouble()) return String(std::to_string(std::get<double>(m_impl->value_)));
            if (IsBool()) return std::get<bool>(m_impl->value_) ? u"true" : u"false";
            if (IsArray()) {
                String s = u"[";
                const auto& arr = std::get<Array>(m_impl->value_);
                for (size_t i = 0; i < arr.size(); ++i) {
                    s += arr[i].AsString(p);
                    if (i + 1 < arr.size()) s += u", ";
                }
                s += u"]";
                return s;
            }
            if (IsObject()) {
                String s = u"{";
                const auto& obj = std::get<ObjectMap>(m_impl->value_);
                size_t i = 0;
                for (const auto& [k, v] : obj) {
                    s.Append(u"\"").Append(k).Append(u"\": ").Append(v.AsString(p));
                    if (++i < obj.size()) s += u", ";
                }
                s += u"}";
                return s;
            }
        }
        throw std::runtime_error("Cannot cast " + TypeName().ToStdString() + " to string");
    }

    const Configuration::Array& Configuration::AsArray(CastPolicy p) const {
        std::shared_lock lock(m_impl->mutex_); // 共享锁
        if (IsArray()) return std::get<Array>(m_impl->value_);

        if (p == CastPolicy::AutoConvert) {
            static thread_local Array temp;
            temp.clear();

            if (IsNull()) {
                // null -> []
                return temp;
            }
            // 其他类型 -> [value]
            temp.push_back(*this);
            return temp;
        }

        throw std::runtime_error("Cannot cast " + TypeName().ToStdString() + " to array");
    }

    Configuration::Array& Configuration::AsArray(CastPolicy p) {
        std::unique_lock lock(m_impl->mutex_); // 独占锁
        if (IsArray()) return std::get<Array>(m_impl->value_);

        if (p == CastPolicy::AutoConvert) {
            Array arr;
            if (!IsNull()) {
                arr.push_back(*this); // 包装成单元素数组
            }
            m_impl->value_ = std::move(arr);
            return std::get<Array>(m_impl->value_);
        }

        throw std::runtime_error("Cannot cast " + TypeName().ToStdString() + " to array");
    }

    const Configuration::ObjectMap& Configuration::AsObject(CastPolicy p) const {
        std::shared_lock lock(m_impl->mutex_); // 共享锁
        if (IsObject()) return std::get<ObjectMap>(m_impl->value_);

        if (p == CastPolicy::AutoConvert) {
            static thread_local ObjectMap temp;
            temp.clear();

            if (IsNull()) {
                // null -> {}
                return temp;
            }
            // 其他类型 -> {"value": this}
            temp[u"value"] = *this;
            return temp;
        }

        throw std::runtime_error("Cannot cast " + TypeName().ToStdString() + " to object");
    }

    Configuration::ObjectMap& Configuration::AsObject(CastPolicy p) {
        std::unique_lock lock(m_impl->mutex_); // 独占锁
        if (IsObject()) return std::get<ObjectMap>(m_impl->value_);

        if (p == CastPolicy::AutoConvert) {
            ObjectMap obj;
            if (!IsNull()) {
                obj[u"value"] = *this; // 包装成 {"value": ...}
            }
            m_impl->value_ = std::move(obj);
            return std::get<ObjectMap>(m_impl->value_);
        }

        throw std::runtime_error("Cannot cast " + TypeName().ToStdString() + " to object");
    }

    void Configuration::Emplace(const String& key, Configuration val) {
        std::unique_lock lock(m_impl->mutex_); // 独占锁
        EnsureObject();
        std::get<ObjectMap>(m_impl->value_)[key] = std::move(val);
    }

    void Configuration::Push_back(Configuration val) {
        std::unique_lock lock(m_impl->mutex_); // 独占锁
        EnsureArray();
        std::get<Array>(m_impl->value_).push_back(std::move(val));
    }

    bool Configuration::operator==(const Configuration& other) const {
        if (this == &other) return true;
        std::shared_lock lhsLock(m_impl->mutex_, std::defer_lock);
        std::shared_lock rhsLock(other.m_impl->mutex_, std::defer_lock);
        std::lock(lhsLock, rhsLock);
        return m_impl->value_ == other.m_impl->value_;
    }

    void Configuration::SetDefaultSerializer(std::shared_ptr<Configuration::Serializer> s) {
        std::unique_lock lock(ConfigurationImpl::g_default_serializer_mutex); // 独占锁
        ConfigurationImpl::defaultSerializer = std::move(s);
    }

    void Configuration::SetSerializer(std::shared_ptr<Configuration::Serializer> s) {
        std::unique_lock lock(m_impl->g_serializer_mutex); // 独占锁
        m_impl->serializer = std::move(s);
    }

    String Configuration::Dump(int indent) const {
        Serializer* serializer = nullptr;
        // 获取自己的序列化器
        {
            std::shared_lock lock(m_impl->g_serializer_mutex);
            serializer = m_impl->serializer.get();
        }
        if (!serializer) { // 没有自己的序列化器，使用全局的序列化器
            std::shared_lock lock(ConfigurationImpl::g_default_serializer_mutex); // 共享锁
            serializer = ConfigurationImpl::defaultSerializer.get();
        }
        if (!serializer) throw std::runtime_error("No serializer registered");

        std::shared_lock lock(m_impl->mutex_); // 共享锁
        return serializer->Serialize(*this, indent);
    }

    void Configuration::Load(const String& text) {
        Serializer* serializer = nullptr;
        // 获取自己的序列化器
        {
            std::shared_lock lock(m_impl->g_serializer_mutex);
            serializer = m_impl->serializer.get();
        }
        if (!serializer) { // 没有自己的序列化器，使用全局的序列化器
            std::shared_lock lock(ConfigurationImpl::g_default_serializer_mutex); // 共享锁
            serializer = ConfigurationImpl::defaultSerializer.get();
        }
        if (!serializer) throw std::runtime_error("No serializer registered");

        Configuration tmp = serializer->Deserialize(text); // 先解析到临时对象
        std::unique_lock writeLock(m_impl->mutex_);       // 修改当前对象的 value_ 需要独占锁
        m_impl->value_ = std::move(tmp.m_impl->value_);           // 替换当前对象的数据
    }

    void Configuration::EnsureObject() {
        if (!IsObject()) m_impl->value_ = ObjectMap{};
    }

    void Configuration::EnsureArray() {
        if (!IsArray()) m_impl->value_ = Array{};
    }

    // Json 序列化器
    class JsonSerializer : public Configuration::Serializer {
    public:
        String Serialize(const Configuration& cfg, int indent = 2) const override {
            std::wostringstream woss;
            SerializeValue(cfg, indent, 0, woss);
            return String(woss.str());
        }

        Configuration Deserialize(const String& text) const override {
            size_t pos = 0;
            Configuration val = ParseValue(text, pos);
            SkipWhitespace(text, pos);
            if (pos != text.Size())
                throw std::runtime_error("Extra data after JSON value");
            return val;
        }

    private:
        // =================== 序列化 ===================
        void SerializeValue(const Configuration& cfg, int indent, int level, std::wostringstream& woss) const {
            String ind;
            if (indent > 0) {
                ind = String(level * indent, u' ');
            }

            // 处理 Null 值
            if (cfg.IsNull()) {
                woss << L"null";
                return;
            }

            // 处理布尔值
            if (cfg.IsBool()) {
                woss << String::FromBool(cfg.AsBool());
                return;
            }

            // 处理整型
            if (cfg.IsInt()) {
                woss << cfg.AsInt();
                return;
            }

            // 处理 64 位整型
            if (cfg.IsInt64()) {
                woss << cfg.AsInt64();
                return;
            }

            // 处理浮点数
            if (cfg.IsDouble()) {
                woss << cfg.AsDouble();
                return;
            }

            // 处理字符串
            if (cfg.IsString()) {
                woss << L"\"";
                woss << String::EscapeJson(cfg.AsString());
                woss << L"\"";
                return;
            }

            // 处理数组
            if (cfg.IsArray()) {
                const auto& arr = cfg.AsArray();
                woss << L"[";

                if (indent != -1) {
                    // 缩进模式（换行和缩进）
                    for (size_t i = 0; i < arr.size(); ++i) {
                        if (i) woss << L",";
                        woss << L"\n" << String((level + 1) * indent, u' ');
                        SerializeValue(arr[i], indent, level + 1, woss);
                    }
                    woss << "\n" << ind << "]";
                }
                else {
                    // 紧凑模式（没有换行）
                    for (size_t i = 0; i < arr.size(); ++i) {
                        if (i) woss << L",";
                        SerializeValue(arr[i], indent, level, woss);
                    }
                    woss << "]";
                }

                return;
            }

            // 处理对象
            if (cfg.IsObject()) {
                const auto& obj = cfg.AsObject();
                woss << L"{";

                if (indent != -1) {
                    // 缩进模式（换行和缩进）
                    size_t count = 0;
                    for (const auto& [k, v] : obj) {
                        if (count++) woss << L",";
                        woss << L"\n" << String((level + 1) * indent, u' ');
                        woss << L"\"";
                        woss << String::EscapeJson(k);
                        woss << L"\": ";
                        SerializeValue(v, indent, level + 1, woss);
                    }
                    woss << L"\n" << ind << L"}";
                }
                else {
                    // 紧凑模式（没有换行）
                    size_t count = 0;
                    for (const auto& [k, v] : obj) {
                        if (count++) woss << L",";
                        woss << L"\"";
                        woss << String::EscapeJson(k);
                        woss << L"\": ";
                        SerializeValue(v, indent, level, woss);
                    }
                    woss << L"}";
                }

                return;
            }

            // 如果遇到无法识别的类型，抛出异常
            throw std::runtime_error("Unknown type in serializeValue");
        }

        // =================== 反序列化 ===================
        static void SkipWhitespace(const String& s, size_t& pos) {
            while (pos < s.Size())
            {
                char32_t ch = s.At(pos);
                if (ch == U' ' ||  // space
                    ch == U'\t' ||  // horizontal tab
                    ch == U'\n' ||  // line feed
                    ch == U'\r')    // carriage return
                {
                    ++pos;
                }
                else
                {
                    break;
                }
            }
        }

        Configuration ParseValue(const String& s, size_t& pos) const {
            SkipWhitespace(s, pos); // 你需要实现一个基于 LikesProgram::String 的 SkipWhitespace

            if (pos >= s.Size())
                throw std::runtime_error("Unexpected end of input");

            char32_t c = s.At(pos);

            if (c == U'{') return ParseObject(s, pos);
            if (c == U'[') return ParseArray(s, pos);
            if (c == U'"') return ParseString(s, pos);
            if ((c >= U'0' && c <= U'9') || c == U'-' || c == U'+') return ParseNumber(s, pos);

            // 使用 SubString + Equals 判断关键字
            if (s.SubString(pos, 4) == LikesProgram::String("true")) { pos += 4; return Configuration(true); }
            if (s.SubString(pos, 5) == LikesProgram::String("false")) { pos += 5; return Configuration(false); }
            if (s.SubString(pos, 4) == LikesProgram::String("null")) { pos += 4; return Configuration(); }

            throw std::runtime_error("Invalid JSON value at position " + std::to_string(pos));
        }

        Configuration ParseObject(const String& s, size_t& pos) const {
            ++pos; // skip '{'
            Configuration::ObjectMap obj;
            SkipWhitespace(s, pos);
            if (pos < s.Size() && s.At(pos) == U'}') { ++pos; return Configuration(std::move(obj)); }

            while (true) {
                SkipWhitespace(s, pos);
                String key = ParseString(s, pos).AsString();
                SkipWhitespace(s, pos);
                if (pos >= s.Size() || s.At(pos) != U':') throw std::runtime_error("Expected ':' in object");
                ++pos;
                Configuration val = ParseValue(s, pos);
                obj[key] = val; // Config::ObjectMap 还是用 std::string 做 key
                SkipWhitespace(s, pos);
                if (pos >= s.Size()) throw std::runtime_error("Unexpected end in object");
                if (s.At(pos) == U'}') { ++pos; break; }
                if (s.At(pos) != U',') throw std::runtime_error("Expected ',' in object");
                ++pos;
            }
            return Configuration(std::move(obj));
        }

        Configuration ParseArray(const String& s, size_t& pos) const {
            ++pos; // skip '['
            Configuration::Array arr;
            SkipWhitespace(s, pos);
            if (pos < s.Size() && s.At(pos) == U']') { ++pos; return Configuration(std::move(arr)); }

            while (true) {
                arr.push_back(ParseValue(s, pos));
                SkipWhitespace(s, pos);
                if (pos >= s.Size()) throw std::runtime_error("Unexpected end in array");
                if (s.At(pos) == U']') { ++pos; break; }
                if (s.At(pos) != U',') throw std::runtime_error("Expected ',' in array");
                ++pos;
            }
            return Configuration(std::move(arr));
        }

        char32_t ParseUnicodeEscape(const String& s, size_t& pos) const {
            if (pos + 4 > s.Size()) throw std::runtime_error("Incomplete \\u escape");
            char32_t code = 0;
            for (int i = 0; i < 4; ++i) {
                char32_t c = s.At(pos++);
                code <<= 4;
                if (c >= U'0' && c <= U'9') code += c - U'0';
                else if (c >= U'a' && c <= U'f') code += c - U'a' + 10;
                else if (c >= U'A' && c <= U'F') code += c - U'A' + 10;
                else throw std::runtime_error("Invalid hex digit in \\u escape");
            }

            if (code >= 0xD800 && code <= 0xDFFF) throw std::runtime_error("Invalid surrogate pair");
            if (code > 0x10FFFF) throw std::runtime_error("Code point exceeds 0x10FFFF");
            return code;
        }

        Configuration ParseString(const String& s, size_t& pos) const {
            if (s.At(pos) != U'"') throw std::runtime_error("Expected '\"'");
            ++pos;
            std::u32string u32res;
            while (pos < s.Size()) {
                char32_t c = s.At(pos++);
                if (c == U'"') break;
                if (c == U'\\') {
                    if (pos >= s.Size()) throw std::runtime_error("Invalid escape");
                    char32_t esc = s.At(pos++);
                    switch (esc) {
                    case U'"': u32res.push_back(U'"'); break;
                    case U'\\': u32res.push_back(U'\\'); break;
                    case U'/': u32res.push_back(U'/'); break;
                    case U'b': u32res.push_back(U'\b'); break;
                    case U'f': u32res.push_back(U'\f'); break;
                    case U'n': u32res.push_back(U'\n'); break;
                    case U'r': u32res.push_back(U'\r'); break;
                    case U't': u32res.push_back(U'\t'); break;
                    case U'u': {
                        char32_t code = ParseUnicodeEscape(s, pos);
                        // 处理 surrogate pair
                        if (code >= 0xD800 && code <= 0xDBFF) {
                            if (pos + 2 >= s.Size() || s.At(pos) != U'\\' || s.At(pos + 1) != U'u')
                                throw std::runtime_error("Expected low surrogate pair");
                            pos += 2;
                            char32_t low = ParseUnicodeEscape(s, pos);
                            if (low < 0xDC00 || low > 0xDFFF)
                                throw std::runtime_error("Invalid low surrogate");
                            code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                        }
                        u32res.push_back(code);
                        break;
                    }
                    default: throw std::runtime_error("Unsupported escape");
                    }
                }
                else u32res.push_back(c);
            }
            return Configuration(String(u32res));
        }

        Configuration ParseNumber(const String& s, size_t& pos) const {
            size_t start = pos;
            char32_t c = s.At(pos);
            if (c == U'-' || c == U'+') ++pos;
            while (pos < s.Size() && std::isdigit(static_cast<unsigned char>(s.At(pos)))) ++pos;
            bool isFloat = false;
            if (pos < s.Size() && s.At(pos) == U'.') {
                isFloat = true; ++pos;
                while (pos < s.Size() && std::isdigit(static_cast<unsigned char>(s.At(pos)))) ++pos;
            }
            if (pos < s.Size() && (s.At(pos) == U'e' || s.At(pos) == U'E')) {
                isFloat = true; ++pos;
                if (pos < s.Size() && (s.At(pos) == U'+' || s.At(pos) == U'-')) ++pos;
                while (pos < s.Size() && std::isdigit(static_cast<unsigned char>(s.At(pos)))) ++pos;
            }

            LikesProgram::String numStr = s.SubString(start, pos - start);
            std::string numStd = numStr.ToStdString();
            if (isFloat) return Configuration(std::stod(numStd));
            try { return Configuration(std::stoi(numStd)); }
            catch (...) { return Configuration(static_cast<int64_t>(std::stoll(numStd))); }
        }
    };

    // JsonSerializer 工厂函数
    std::shared_ptr<Configuration::Serializer> Configuration::CreateJsonSerializer() {
        return std::make_shared<JsonSerializer>();
    }
}
