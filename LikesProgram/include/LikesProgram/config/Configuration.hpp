#pragma once
#include "../String.hpp"
#include <map>
#include <vector>
#include <variant>
#include <memory>
#include <shared_mutex>

namespace LikesProgram {
    namespace Config {
        class Configuration {
        public:
            using ObjectMap = std::map<LikesProgram::String, Configuration>;
            using Array = std::vector<Configuration>;
            using Value = std::variant<std::monostate, int, int64_t, double, bool, LikesProgram::String, Array, ObjectMap>;

            enum class CastPolicy { Strict, AutoConvert };

            // -------- 构造与赋值 --------
            Configuration() = default;
            Configuration(int v) : value_(v) {}
            Configuration(int64_t v) : value_(v) {}
            Configuration(double v) : value_(v) {}
            Configuration(bool v) : value_(v) {}
            Configuration(const char* v) : value_(LikesProgram::String(v)) {}
            Configuration(LikesProgram::String v) : value_(std::move(v)) {}
            Configuration(Array v) : value_(std::move(v)) {}
            Configuration(ObjectMap v) : value_(std::move(v)) {}
            Configuration(const Configuration& other); // 拷贝构造：复制值，创建新锁
            Configuration(Configuration&& other) noexcept; // 移动构造

            ~Configuration() = default;

            Configuration& operator=(int v) { value_ = v; return *this; }
            Configuration& operator=(int64_t v) { value_ = v; return *this; }
            Configuration& operator=(double v) { value_ = v; return *this; }
            Configuration& operator=(bool v) { value_ = v; return *this; }
            Configuration& operator=(const LikesProgram::String& v) { value_ = v; return *this; }
            Configuration& operator=(const char* v) { value_ = LikesProgram::String(v); return *this; }
            Configuration& operator=(Array v) { value_ = std::move(v); return *this; }
            Configuration& operator=(ObjectMap v) { value_ = std::move(v); return *this; }
            Configuration& operator=(const Configuration& other); // 拷贝赋值：复制值，创建新锁
            Configuration& operator=(Configuration&& other) noexcept; // 移动赋值

            // -------- 类型判断 --------
            bool IsNull()   const { return std::holds_alternative<std::monostate>(value_); }
            bool IsInt()    const { return std::holds_alternative<int>(value_); }
            bool IsInt64() const { return std::holds_alternative<int64_t>(value_); }
            bool IsDouble() const { return std::holds_alternative<double>(value_); }
            bool IsBool()   const { return std::holds_alternative<bool>(value_); }
            bool IsString() const { return std::holds_alternative<LikesProgram::String>(value_); }
            bool IsArray()  const { return std::holds_alternative<Array>(value_); }
            bool IsObject() const { return std::holds_alternative<ObjectMap>(value_); }
            bool IsNumber() const { return IsInt() || IsInt64() || IsDouble(); }

            LikesProgram::String TypeName() const;

            // -------- 访问 --------
            Configuration& operator[](const LikesProgram::String& key);
            const Configuration& operator[](const LikesProgram::String& key) const;
            // 数组访问运算符
            Configuration& operator[](size_t idx);
            const Configuration& operator[](size_t idx) const;

            Configuration& At(size_t idx);
            const Configuration& At(size_t idx) const;
            size_t Size() const;

            // -------- 迭代器支持 --------
            // 数组迭代器
            Array::iterator begin() { EnsureArray(); return std::get<Array>(value_).begin(); }
            Array::iterator end() { EnsureArray(); return std::get<Array>(value_).end(); }
            Array::const_iterator begin() const { return std::get<Array>(value_).begin(); }
            Array::const_iterator end()   const { return std::get<Array>(value_).end(); }

            // 对象迭代器
            ObjectMap::iterator beginObject() { EnsureObject(); return std::get<ObjectMap>(value_).begin(); }
            ObjectMap::iterator endObject() { EnsureObject(); return std::get<ObjectMap>(value_).end(); }
            ObjectMap::const_iterator beginObject() const { return std::get<ObjectMap>(value_).begin(); }
            ObjectMap::const_iterator endObject()   const { return std::get<ObjectMap>(value_).end(); }

            // 支持范围 for 遍历对象
            struct ObjectRange {
                ObjectMap::iterator beginIt;
                ObjectMap::iterator endIt;
                ObjectRange(ObjectMap::iterator b, ObjectMap::iterator e) : beginIt(b), endIt(e) {}
                ObjectMap::iterator begin() { return beginIt; }
                ObjectMap::iterator end() { return endIt; }
            };

            struct ConstObjectRange {
                ObjectMap::const_iterator beginIt;
                ObjectMap::const_iterator endIt;
                ConstObjectRange(ObjectMap::const_iterator b, ObjectMap::const_iterator e) : beginIt(b), endIt(e) {}
                ObjectMap::const_iterator begin() const { return beginIt; }
                ObjectMap::const_iterator end() const { return endIt; }
            };

            // 返回对象范围
            ObjectRange objects() { EnsureObject(); return ObjectRange(std::get<ObjectMap>(value_).begin(), std::get<ObjectMap>(value_).end()); }
            ConstObjectRange objects() const { return ConstObjectRange(std::get<ObjectMap>(value_).begin(), std::get<ObjectMap>(value_).end()); }

            // -------- 值转换 --------
            int AsInt(CastPolicy p = CastPolicy::AutoConvert) const;

            int64_t AsInt64(CastPolicy p = CastPolicy::AutoConvert) const;

            double AsDouble(CastPolicy p = CastPolicy::AutoConvert) const;

            bool AsBool(CastPolicy p = CastPolicy::AutoConvert) const;

            LikesProgram::String AsString(CastPolicy p = CastPolicy::AutoConvert) const;

            const Array& AsArray(CastPolicy p = CastPolicy::Strict) const;

            Array& AsArray(CastPolicy p = CastPolicy::Strict);

            const ObjectMap& AsObject(CastPolicy p = CastPolicy::Strict) const;

            ObjectMap& AsObject(CastPolicy p = CastPolicy::Strict);

            template <typename T>
            bool TryGet(T& out, CastPolicy p = CastPolicy::AutoConvert) const {
                try {
                    if constexpr (std::is_same_v<T, int>) out = AsInt(p);
                    else if constexpr (std::is_same_v<T, int64_t>) out = AsInt64(p);
                    else if constexpr (std::is_same_v<T, double>) out = AsDouble(p);
                    else if constexpr (std::is_same_v<T, bool>) out = AsBool(p);
                    else if constexpr (std::is_same_v<T, LikesProgram::String>) out = AsString(p);
                    else return false;
                    return true;
                }
                catch (...) {
                    return false;
                }
            }

            // -------- 添加修改 --------
            void Emplace(const LikesProgram::String& key, Configuration val);
            void Push_back(Configuration val);

            // -------- 比较 --------
            bool operator==(const Configuration& other) const;
            bool operator!=(const Configuration& other) const { return !(*this == other); }

            // -------- Serializer 接口 --------
            class Serializer {
            public:
                virtual ~Serializer() = default;
                virtual LikesProgram::String Serialize(const Configuration& cfg, int indent = 2) const = 0;
                virtual Configuration Deserialize(const LikesProgram::String& text) const = 0;
            };

            static void SetDefaultSerializer(std::unique_ptr<Serializer> s) {
                std::lock_guard lock(g_serializer_mutex);
                DefaultSerializer() = std::move(s);
            }

            LikesProgram::String Dump(int indent = 2) const {
                std::lock_guard lock(g_serializer_mutex);
                auto* s = DefaultSerializer().get();
                if (!s) throw std::runtime_error("No serializer registered");
                return s->Serialize(*this, indent);
            }

            static Configuration Load(const LikesProgram::String& text) {
                std::lock_guard lock(g_serializer_mutex);
                auto* s = DefaultSerializer().get();
                if (!s) throw std::runtime_error("No serializer registered");
                return s->Deserialize(text);
            }

        private:
            Value value_;

            mutable std::shared_mutex mutex_;  // 每个对象独有

            static std::mutex g_serializer_mutex; // 全局锁

            void EnsureObject() {
                if (!IsObject()) value_ = ObjectMap{};
            }

            void EnsureArray() {
                if (!IsArray()) value_ = Array{};
            }

            // 全局序列化器
            static std::unique_ptr<Serializer>& DefaultSerializer();
        };
    }
}
