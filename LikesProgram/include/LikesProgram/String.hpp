﻿#pragma once
#include "LikesProgramLibExport.hpp"
#include <vector>
#include <string>
#include <memory>

namespace LikesProgram {
    class LIKESPROGRAM_API String {
    public:
        // 支持的编码类型，仅用于标识字符串来源，内部存储总是 UTF-16。
        enum class Encoding { GBK, UTF8, UTF16, UTF32 };
        static constexpr size_t npos = static_cast<size_t>(-1);

        String();
        // char 未指定编码，默认为 UTF-8
        explicit String(const char* s, Encoding enc = Encoding::UTF8);
        // UTF-8
        String(const char8_t* s);
        // UTF-16
        String(const char16_t* s);
        // UTF-32
        String(const char32_t* s);
        // 拷贝构造
        String(const String& other);
        // 移动构造
        String(String&& other) noexcept;
        // 构造单个字符
        String(const char c, Encoding enc = Encoding::UTF8);
        String(const char8_t c);
        String(const char16_t c);
        String(const size_t count, const char16_t c);
        String(const char32_t c);
        // 析构函数
        ~String();

        // 赋值操作
        String& operator=(const String& other);
        // 赋值操作
        String& operator=(String&& other) noexcept;

        // 返回字符数（Unicode aware）
        size_t Size() const;
        // 返回字符数（Unicode aware）
        size_t Length() const;
        // 是否为空
        bool Empty() const;
        // 清空字符串
        void Clear();

        // 安全访问字符（Unicode aware）
        char32_t At(size_t index) const;
        // 字符串第一个字符（Unicode aware）
        char32_t Front() const;
        // 字符串最后一个字符（Unicode aware）
        char32_t Back() const;

        // 拼接字符串
        String& Append(const String& str);
        String& operator+=(const String& str);
        friend String operator+(const String& lhs, const String& rhs);

        // 输入输出重载
        friend std::ostream& operator<<(std::ostream& os, const String& str);
        friend std::istream& operator>>(std::istream& is, String& str);
        friend std::wostream& operator<<(std::wostream& os, const String& str);
        friend std::wistream& operator>>(std::wistream& is, String& str);

        // 获取子串
        String SubString(size_t index, size_t count) const;
        // 左侧截取
        String Left(size_t count) const;
        // 右侧截取
        String Right(size_t count) const;

        // 转换为大写
        String ToUpper() const;
        // 转换为小写
        String ToLower() const;
        // 转换为大写（直接转换）
        void ToUpperInPlace();
        // 转换为小写（直接转换）
        void ToLowerInPlace();

        // 查找子串
        size_t Find(const String& str, size_t start = 0) const;
        // 反向查找子串
        size_t LastFind(const String& str, size_t start = 0) const;
        // 判断是否以指定子串开头
        bool StartsWith(const String& str) const;
        // 判断是否以指定子串结尾
        bool EndsWith(const String& str) const;

        // 忽略大小写判断是否相等
        bool EqualsIgnoreCase(const String& other) const;
        bool operator==(const String& other) const;
        bool operator!=(const String& other) const;
        bool operator<(const String& other) const;
        bool operator<=(const String& other) const;
        bool operator>(const String& other) const;
        bool operator>=(const String& other) const;

        // 转换为string
        std::string ToStdString(Encoding enc = Encoding::UTF8) const;
        // 转换为wstring
        std::wstring ToWString() const;
        // 转换为u16string
        std::u16string ToU16String() const;
        // 转换为u32string
        std::u32string ToU32String() const;
        // string 转换为String
        explicit String(const std::string& s, Encoding enc = Encoding::UTF8);
        // u8string 转换为String
        explicit String(const std::u8string& s);
        // wstring 转换为String
        explicit String(const std::wstring& s);
        // u16string 转换为String
        explicit String(const std::u16string& s);
        // u32string 转换为String
        explicit String(const std::u32string& s);

        // 分割成字符串数组
        std::vector<String> Split(const String& sep) const;

        // 迭代器
        class CodePointIterator {
        private:
            const String* str;
            size_t idx;  // code point 索引
        public:
            CodePointIterator(const String* s, size_t i) : str(s), idx(i) {}
            char32_t operator*() const { return str->At(idx); }
            CodePointIterator& operator++() { ++idx; return *this; }
            bool operator!=(const CodePointIterator& other) const { return idx != other.idx; }
        };

        CodePointIterator begin() const { return CodePointIterator(this, 0); }
        CodePointIterator end() const { return CodePointIterator(this, Size()); }


        // JSON 转义
        static String EscapeJson(const String& str);
        static String FromInt(int64_t value);
        static String FromUInt(uint64_t value);
        static String FromFloat(double value, size_t precision = 0);
        static String FromBool(bool value);

    private:
        struct StringImpl;
        StringImpl* m_impl = nullptr;

        size_t CodePointOffset(size_t index) const;

        void update_cp_cache() const;
    };
}

namespace std {
    template<>
    struct hash<LikesProgram::String> {
        size_t operator()(const LikesProgram::String& s) const noexcept {
            size_t h = 0;
            for (auto cp : s) h = h * 31 + static_cast<size_t>(cp);
            return h;
        }
    };
}
