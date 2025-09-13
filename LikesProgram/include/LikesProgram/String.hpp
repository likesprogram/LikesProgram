//#pragma once
//#include "LikesProgramLibExport.hpp"
//#include <vector>
//#include <string>
//
//namespace LikesProgram {
//    class LIKESPROGRAM_API String {
//    public:
//        // ֧�ֵı������ͣ������ڱ�ʶ�ַ�����Դ���ڲ��洢���� UTF-8��
//        enum class Encoding { GBK, UTF8, UTF16, UTF32 };
//        static constexpr size_t npos = static_cast<size_t>(-1);
//
//        String();
//        // char δָ�����룬Ĭ��Ϊ UTF-8
//        explicit String(const char* s, Encoding enc = Encoding::UTF8);
//        // UTF-8
//        explicit String(const char8_t* s);
//        // UTF-16
//        explicit String(const char16_t* s);
//        // UTF-32
//        explicit String(const char32_t* s);
//        // ��������
//        String(const String& other);
//        // �ƶ�����
//        String(String&& other) noexcept;
//        // ���쵥���ַ�
//        explicit String(const char c);
//        explicit String(const char16_t c);
//        explicit String(const char32_t c);
//        // ��������
//        ~String();
//
//        // ��ֵ����
//        String& operator=(const String& other);
//        // ��ֵ����
//        String& operator=(String&& other) noexcept;
//
//        // �����ַ�����Unicode aware��
//        size_t Size() const;
//        // �����ַ�����Unicode aware��
//        size_t Length() const;
//        // �Ƿ�Ϊ��
//        bool Empty() const;
//        // ����ַ���
//        void Clear();
//        // Ԥ�����ڴ�
//        void Reserve(size_t size);
//
//        // ֱ�ӷ����ַ���Unicode aware��
//        char32_t operator[](size_t index) const;
//
//        // ��ȫ�����ַ���Unicode aware��
//        char32_t At(size_t index) const;
//        // �ַ�����һ���ַ���Unicode aware��
//        char32_t Front() const;
//        // �ַ������һ���ַ���Unicode aware��
//        char32_t Back() const;
//
//        // ƴ���ַ���
//        String& Append(const String& str);
//        String& operator+=(const String& str);
//        // ǰ��ƴ���ַ���
//        String& Prepend(const String& str);
//        // �����Ӵ�
//        String& Insert(size_t index, const String& str);
//        // ɾ���Ӵ�
//        String& Remove(size_t index, size_t count);
//        // �滻�Ӵ�
//        String& Replace(size_t index, size_t count, const String& str);
//
//        // ��ȡ�Ӵ�
//        String SubString(size_t index, size_t count) const;
//        // ����ȡ
//        String Left(size_t count) const;
//        // �Ҳ��ȡ
//        String Right(size_t count) const;
//
//        // ת��Ϊ��д (��ת��ASCII�Ĵ�д��ĸ)
//        String ToUpper() const;
//        // ת��ΪСд (��ת��ASCII�Ĵ�д��ĸ)
//        String ToLower() const;
//        // ת��Ϊ��д��ֱ��ת����(��ת��ASCII�Ĵ�д��ĸ)
//        void ToUpperInPlace();
//        // ת��ΪСд��ֱ��ת����(��ת��ASCII�Ĵ�д��ĸ)
//        void ToLowerInPlace();
//
//        // �����Ӵ�
//        size_t Find(const String& str, size_t start = 0) const;
//        // ���Դ�Сд�ж��Ƿ����ָ���Ӵ�
//        size_t FindIgnoreCase(const String& substr, size_t start = 0) const;
//        // ��������Ӵ�
//        size_t LastFind(const String& str, size_t start = 0) const;
//        // ���Դ�Сд�ж��Ƿ���ָ���Ӵ���ͷ
//        size_t LastFindIgnoreCase(const String& substr, size_t start = 0) const;
//        // ͳ���Ӵ����ֵĴ���
//        size_t Countains(const String& str) const;
//        // ���Դ�Сдͳ���Ӵ����ֵĴ���
//        size_t CountainsIgnoreCase(const String& str) const;
//        // �ж��Ƿ���ָ���Ӵ���ͷ
//        bool StartsWith(const String& str) const;
//        // ���Դ�Сд�ж��Ƿ���ָ���Ӵ���ͷ
//        bool StartsWithIgnoreCase(const String& prefix) const;
//        // �ж��Ƿ���ָ���Ӵ���β
//        bool EndsWith(const String& str) const;
//        // ���Դ�Сд�ж��Ƿ���ָ���Ӵ���β
//        bool EndsWithIgnoreCase(const String& suffix) const;
//        // �Ƿ����ָ���Ӵ�
//        bool Contains(const String& str) const;
//        // ���Դ�Сд�ж��Ƿ����ָ���Ӵ�
//        bool ContainsIgnoreCase(const String& substr) const;
//
//        // �滻ȫ���Ӵ�
//        String& ReplaceAll(const String& str);
//        // ���Դ�Сд�滻ȫ���Ӵ�
//        String& ReplaceAllIgnoreCase(const String& str);
//
//        // �����ַ����Ĺ�ϣֵ
//        size_t Hash() const;
//        // ���Դ�Сд�����ַ����Ĺ�ϣֵ
//        size_t HashIgnoreCase() const;
//
//        // ���Դ�Сд�ж��Ƿ����
//        bool EqualsIgnoreCase(const String& other) const;
//        bool operator==(const String& other) const;
//        bool operator!=(const String& other) const;
//        bool operator<(const String& other) const;
//        bool operator<=(const String& other) const;
//        bool operator>(const String& other) const;
//        bool operator>=(const String& other) const;
//        String operator+(const String& other) const;
//
//        // ת��Ϊstring
//        std::string ToStdString(Encoding enc = Encoding::UTF8) const;
//        // ת��Ϊwstring
//        std::wstring ToWString() const;
//        // ת��Ϊu16string
//        std::u16string ToU16String() const;
//        // ת��Ϊu32string
//        std::u32string ToU32String() const;
//        // string ת��ΪString
//        explicit String(const std::string& s, Encoding enc = Encoding::UTF8);
//        // wstring ת��ΪString
//        explicit String(const std::wstring& s);
//        // u16string ת��ΪString
//        explicit String(const std::u16string& s);
//        // u32string ת��ΪString
//        explicit String(const std::u32string& s);
//
//        // ������
//        class Iterator { };
//        Iterator begin() const;
//        Iterator end() const;
//
//        // ��ʽ��
//        String Format(const String& format, ...) const;
//        String operator()(const String& format, ...) const;
//        // ���
//        friend std::ostream& operator<<(std::ostream& os, const String& str);
//        friend std::wostream& operator<<(std::wostream& os, const String& str);
//        // ����
//        friend std::istream& operator>>(std::istream& is, String& str);
//        friend std::wistream& operator>>(std::wistream& is, String& str);
//
//        // ��ȡC����ַ���
//        const char* c_str() const;
//
//        // �ָ���ַ�������
//        std::vector<String> Split(const String& sep) const;
//        // ȥ����β�ո�
//        String Trimmed() const;
//        // ȥ���ո�
//        String& Trim();
//        // ѹ�������ո�
//        String Simplified() const;
//        // ����ת�ַ���
//        static String Number(int n);
//        static String Number(int64_t n);
//        static String Number(double d);
//
//        // �� UTF-8 ת��Ϊ UTF-16
//        static std::u16string Utf8ToUtf16(const std::string& utf8);
//        // �� UTF-32 ת��Ϊ UTF-16
//        static std::u16string Utf32ToUtf16(const std::u32string& utf32);
//        // �� GBK ת��Ϊ UTF-16
//        static std::u16string GbkToUtf16(const std::string& gbk);
//        // �� UTF-16 ת��Ϊ UTF-8
//        static std::string Utf16ToUtf8(const std::u16string& utf16);
//        // �� UTF-16 ת��Ϊ UTF-32
//        static std::u32string Utf16ToUtf32(const std::u16string& utf16);
//        // �� UTF-16 ת��Ϊ GBK
//        static std::string Utf16ToGbk(const std::u16string& utf16);
//
//    private:
//        // COW �����߼�
//        void Detach();
//        // ȷ����������
//        void EnsureCapacity(size_t required);
//        struct StringData;
//        StringData* m_data;
//    };
//}