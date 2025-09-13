//#pragma once
//#include "LikesProgramLibExport.hpp"
//#include <vector>
//#include <string>
//
//namespace LikesProgram {
//    class LIKESPROGRAM_API String {
//    public:
//        // 支持的编码类型，仅用于标识字符串来源，内部存储总是 UTF-8。
//        enum class Encoding { GBK, UTF8, UTF16, UTF32 };
//        static constexpr size_t npos = static_cast<size_t>(-1);
//
//        String();
//        // char 未指定编码，默认为 UTF-8
//        explicit String(const char* s, Encoding enc = Encoding::UTF8);
//        // UTF-8
//        explicit String(const char8_t* s);
//        // UTF-16
//        explicit String(const char16_t* s);
//        // UTF-32
//        explicit String(const char32_t* s);
//        // 拷贝构造
//        String(const String& other);
//        // 移动构造
//        String(String&& other) noexcept;
//        // 构造单个字符
//        explicit String(const char c);
//        explicit String(const char16_t c);
//        explicit String(const char32_t c);
//        // 析构函数
//        ~String();
//
//        // 赋值操作
//        String& operator=(const String& other);
//        // 赋值操作
//        String& operator=(String&& other) noexcept;
//
//        // 返回字符数（Unicode aware）
//        size_t Size() const;
//        // 返回字符数（Unicode aware）
//        size_t Length() const;
//        // 是否为空
//        bool Empty() const;
//        // 清空字符串
//        void Clear();
//        // 预分配内存
//        void Reserve(size_t size);
//
//        // 直接访问字符（Unicode aware）
//        char32_t operator[](size_t index) const;
//
//        // 安全访问字符（Unicode aware）
//        char32_t At(size_t index) const;
//        // 字符串第一个字符（Unicode aware）
//        char32_t Front() const;
//        // 字符串最后一个字符（Unicode aware）
//        char32_t Back() const;
//
//        // 拼接字符串
//        String& Append(const String& str);
//        String& operator+=(const String& str);
//        // 前置拼接字符串
//        String& Prepend(const String& str);
//        // 插入子串
//        String& Insert(size_t index, const String& str);
//        // 删除子串
//        String& Remove(size_t index, size_t count);
//        // 替换子串
//        String& Replace(size_t index, size_t count, const String& str);
//
//        // 获取子串
//        String SubString(size_t index, size_t count) const;
//        // 左侧截取
//        String Left(size_t count) const;
//        // 右侧截取
//        String Right(size_t count) const;
//
//        // 转换为大写 (仅转换ASCII的大写字母)
//        String ToUpper() const;
//        // 转换为小写 (仅转换ASCII的大写字母)
//        String ToLower() const;
//        // 转换为大写（直接转换）(仅转换ASCII的大写字母)
//        void ToUpperInPlace();
//        // 转换为小写（直接转换）(仅转换ASCII的大写字母)
//        void ToLowerInPlace();
//
//        // 查找子串
//        size_t Find(const String& str, size_t start = 0) const;
//        // 忽略大小写判断是否包含指定子串
//        size_t FindIgnoreCase(const String& substr, size_t start = 0) const;
//        // 反向查找子串
//        size_t LastFind(const String& str, size_t start = 0) const;
//        // 忽略大小写判断是否以指定子串开头
//        size_t LastFindIgnoreCase(const String& substr, size_t start = 0) const;
//        // 统计子串出现的次数
//        size_t Countains(const String& str) const;
//        // 忽略大小写统计子串出现的次数
//        size_t CountainsIgnoreCase(const String& str) const;
//        // 判断是否以指定子串开头
//        bool StartsWith(const String& str) const;
//        // 忽略大小写判断是否以指定子串开头
//        bool StartsWithIgnoreCase(const String& prefix) const;
//        // 判断是否以指定子串结尾
//        bool EndsWith(const String& str) const;
//        // 忽略大小写判断是否以指定子串结尾
//        bool EndsWithIgnoreCase(const String& suffix) const;
//        // 是否包含指定子串
//        bool Contains(const String& str) const;
//        // 忽略大小写判断是否包含指定子串
//        bool ContainsIgnoreCase(const String& substr) const;
//
//        // 替换全部子串
//        String& ReplaceAll(const String& str);
//        // 忽略大小写替换全部子串
//        String& ReplaceAllIgnoreCase(const String& str);
//
//        // 计算字符串的哈希值
//        size_t Hash() const;
//        // 忽略大小写计算字符串的哈希值
//        size_t HashIgnoreCase() const;
//
//        // 忽略大小写判断是否相等
//        bool EqualsIgnoreCase(const String& other) const;
//        bool operator==(const String& other) const;
//        bool operator!=(const String& other) const;
//        bool operator<(const String& other) const;
//        bool operator<=(const String& other) const;
//        bool operator>(const String& other) const;
//        bool operator>=(const String& other) const;
//        String operator+(const String& other) const;
//
//        // 转换为string
//        std::string ToStdString(Encoding enc = Encoding::UTF8) const;
//        // 转换为wstring
//        std::wstring ToWString() const;
//        // 转换为u16string
//        std::u16string ToU16String() const;
//        // 转换为u32string
//        std::u32string ToU32String() const;
//        // string 转换为String
//        explicit String(const std::string& s, Encoding enc = Encoding::UTF8);
//        // wstring 转换为String
//        explicit String(const std::wstring& s);
//        // u16string 转换为String
//        explicit String(const std::u16string& s);
//        // u32string 转换为String
//        explicit String(const std::u32string& s);
//
//        // 迭代器
//        class Iterator { };
//        Iterator begin() const;
//        Iterator end() const;
//
//        // 格式化
//        String Format(const String& format, ...) const;
//        String operator()(const String& format, ...) const;
//        // 输出
//        friend std::ostream& operator<<(std::ostream& os, const String& str);
//        friend std::wostream& operator<<(std::wostream& os, const String& str);
//        // 输入
//        friend std::istream& operator>>(std::istream& is, String& str);
//        friend std::wistream& operator>>(std::wistream& is, String& str);
//
//        // 获取C风格字符串
//        const char* c_str() const;
//
//        // 分割成字符串数组
//        std::vector<String> Split(const String& sep) const;
//        // 去除首尾空格
//        String Trimmed() const;
//        // 去除空格
//        String& Trim();
//        // 压缩连续空格
//        String Simplified() const;
//        // 数字转字符串
//        static String Number(int n);
//        static String Number(int64_t n);
//        static String Number(double d);
//
//        // 将 UTF-8 转换为 UTF-16
//        static std::u16string Utf8ToUtf16(const std::string& utf8);
//        // 将 UTF-32 转换为 UTF-16
//        static std::u16string Utf32ToUtf16(const std::u32string& utf32);
//        // 将 GBK 转换为 UTF-16
//        static std::u16string GbkToUtf16(const std::string& gbk);
//        // 将 UTF-16 转换为 UTF-8
//        static std::string Utf16ToUtf8(const std::u16string& utf16);
//        // 将 UTF-16 转换为 UTF-32
//        static std::u32string Utf16ToUtf32(const std::u16string& utf16);
//        // 将 UTF-16 转换为 GBK
//        static std::string Utf16ToGbk(const std::u16string& utf16);
//
//    private:
//        // COW 分离逻辑
//        void Detach();
//        // 确保容量够用
//        void EnsureCapacity(size_t required);
//        struct StringData;
//        StringData* m_data;
//    };
//}