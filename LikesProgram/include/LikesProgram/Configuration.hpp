#pragma once
#include "system/LikesProgramLibExport.hpp"
#include "String.hpp"
#include <map>
#include <vector>
#include <variant>
#include <memory>

namespace LikesProgram {
    class LIKESPROGRAM_API Configuration {
    public:
        using ObjectMap = std::map<String, Configuration>;
        using Array = std::vector<Configuration>;
        using Value = std::variant<std::monostate, int, int64_t, double, bool, String, Array, ObjectMap>;

        enum class CastPolicy { Strict, AutoConvert };

        // 构造与赋值
        Configuration();
        Configuration(int v);
        Configuration(int64_t v);
        Configuration(double v);
        Configuration(bool v);
        Configuration(const char* v);
        Configuration(const char8_t* v);
        Configuration(const char16_t* v);
        Configuration(const char32_t* v);
        Configuration(const char8_t c);
        Configuration(const char16_t c);
        Configuration(const char32_t c);
        Configuration(String v);
        Configuration(Array v);
        Configuration(ObjectMap v);
        Configuration(const Configuration& other); // 拷贝构造：复制值，创建新锁
        Configuration(Configuration&& other) noexcept; // 移动构造

        ~Configuration();

        Configuration& operator=(int v);
        Configuration& operator=(int64_t v);
        Configuration& operator=(double v);
        Configuration& operator=(bool v);
        Configuration& operator=(const String& v);
        Configuration& operator=(const char* v);
        Configuration& operator=(const char8_t* v);
        Configuration& operator=(const char16_t* v);
        Configuration& operator=(const char32_t* v);
        Configuration& operator=(const char8_t c);
        Configuration& operator=(const char16_t c);
        Configuration& operator=(const char32_t c);
        Configuration& operator=(Array v);
        Configuration& operator=(ObjectMap v);
        Configuration& operator=(const Configuration& other); // 拷贝赋值：复制值，创建新锁
        Configuration& operator=(Configuration&& other) noexcept; // 移动赋值

        // 类型判断
        bool IsNull()   const;
        bool IsInt()    const;
        bool IsInt64()  const;
        bool IsDouble() const;
        bool IsBool()   const;
        bool IsString() const;
        bool IsArray()  const;
        bool IsObject() const;
        bool IsNumber() const;

        String TypeName() const;

        // 访问
        Configuration& operator[](const String& key);
        const Configuration& operator[](const String& key) const;
        // 数组访问运算符
        Configuration& operator[](size_t idx);
        const Configuration& operator[](size_t idx) const;

        Configuration& At(size_t idx);
        const Configuration& At(size_t idx) const;
        size_t Size() const;

        // 迭代器支持
        // 数组迭代器
        Array::iterator begin();
        Array::iterator end();
        Array::const_iterator begin() const;
        Array::const_iterator end() const;

        // 对象迭代器
        ObjectMap::iterator beginObject();
        ObjectMap::iterator endObject();
        ObjectMap::const_iterator beginObject() const;
        ObjectMap::const_iterator endObject() const;

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
        ObjectRange objects();
        ConstObjectRange objects() const;

        // 值转换
        int AsInt(CastPolicy p = CastPolicy::AutoConvert) const;

        int64_t AsInt64(CastPolicy p = CastPolicy::AutoConvert) const;

        double AsDouble(CastPolicy p = CastPolicy::AutoConvert) const;

        bool AsBool(CastPolicy p = CastPolicy::AutoConvert) const;

        String AsString(CastPolicy p = CastPolicy::AutoConvert) const;

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
                else if constexpr (std::is_same_v<T, String>) out = AsString(p);
                else return false;
                return true;
            }
            catch (...) {
                return false;
            }
        }

        // 添加修改
        void Emplace(const String& key, Configuration val);
        void Push_back(Configuration val);

        // 比较
        bool operator==(const Configuration& other) const;
        bool operator!=(const Configuration& other) const { return !(*this == other); }

        // Serializer 接口
        class Serializer {
        public:
            virtual ~Serializer() = default;
            virtual String Serialize(const Configuration& cfg, int indent = 2) const = 0;
            virtual Configuration Deserialize(const String& text) const = 0;
        };

        static void SetDefaultSerializer(std::shared_ptr<Configuration::Serializer> s);

        void SetSerializer(std::shared_ptr<Configuration::Serializer> s);

        String Dump(int indent = 2) const;

        void Load(const String& text);

    private:
        struct ConfigurationImpl;
        ConfigurationImpl* m_impl;

        void EnsureObject();

        void EnsureArray();
    
        // 内置工厂
    public:
        // 创建一个 Json 序列化器
        static std::shared_ptr<Configuration::Serializer> CreateJsonSerializer();
    };
}
