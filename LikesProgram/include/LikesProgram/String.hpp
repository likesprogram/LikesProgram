#pragma once
#include "LikesProgramLibExport.hpp"
#include <vector>
#include <string>
#include <memory>

namespace LikesProgram {
    class LIKESPROGRAM_API String {
    public:
        // ֧�ֵı������ͣ������ڱ�ʶ�ַ�����Դ���ڲ��洢���� UTF-16��
        enum class Encoding { GBK, UTF8, UTF16, UTF32 };
        static constexpr size_t npos = static_cast<size_t>(-1);

        String();
        // char δָ�����룬Ĭ��Ϊ UTF-8
        explicit String(const char* s, Encoding enc = Encoding::UTF8);
        // UTF-8
        explicit String(const char8_t* s);
        // UTF-16
        explicit String(const char16_t* s);
        // UTF-32
        explicit String(const char32_t* s);
        // ��������
        String(const String& other);
        // �ƶ�����
        String(String&& other) noexcept;
        // ���쵥���ַ�
        explicit String(const char8_t c);
        explicit String(const char16_t c);
        explicit String(const char32_t c);
        // ��������
        ~String();

        // ��ֵ����
        String& operator=(const String& other);
        // ��ֵ����
        String& operator=(String&& other) noexcept;

        // �����ַ�����Unicode aware��
        size_t Size() const;
        // �����ַ�����Unicode aware��
        size_t Length() const;
        // �Ƿ�Ϊ��
        bool Empty() const;
        // ����ַ���
        void Clear();

        // ��ȫ�����ַ���Unicode aware��
        char32_t At(size_t index) const;
        // �ַ�����һ���ַ���Unicode aware��
        char32_t Front() const;
        // �ַ������һ���ַ���Unicode aware��
        char32_t Back() const;

        // ƴ���ַ���
        String& Append(const String& str);
        String& operator+=(const String& str);

        // ��ȡ�Ӵ�
        String SubString(size_t index, size_t count) const;
        // ����ȡ
        String Left(size_t count) const;
        // �Ҳ��ȡ
        String Right(size_t count) const;

        // ת��Ϊ��д
        String ToUpper() const;
        // ת��ΪСд
        String ToLower() const;
        // ת��Ϊ��д��ֱ��ת����
        void ToUpperInPlace();
        // ת��ΪСд��ֱ��ת����
        void ToLowerInPlace();

        // �����Ӵ�
        size_t Find(const String& str, size_t start = 0) const;
        // ��������Ӵ�
        size_t LastFind(const String& str, size_t start = 0) const;
        // �ж��Ƿ���ָ���Ӵ���ͷ
        bool StartsWith(const String& str) const;
        // �ж��Ƿ���ָ���Ӵ���β
        bool EndsWith(const String& str) const;

        // ���Դ�Сд�ж��Ƿ����
        bool EqualsIgnoreCase(const String& other) const;
        bool operator==(const String& other) const;
        bool operator!=(const String& other) const;
        bool operator<(const String& other) const;
        bool operator<=(const String& other) const;
        bool operator>(const String& other) const;
        bool operator>=(const String& other) const;

        // ת��Ϊstring
        std::string ToStdString(Encoding enc = Encoding::UTF8) const;
        // ת��Ϊwstring
        std::wstring ToWString() const;
        // ת��Ϊu16string
        std::u16string ToU16String() const;
        // ת��Ϊu32string
        std::u32string ToU32String() const;
        // string ת��ΪString
        explicit String(const std::string& s, Encoding enc = Encoding::UTF8);
        // u8string ת��ΪString
        explicit String(const std::u8string& s);
        // wstring ת��ΪString
        explicit String(const std::wstring& s);
        // u16string ת��ΪString
        explicit String(const std::u16string& s);
        // u32string ת��ΪString
        explicit String(const std::u32string& s);

        // �ָ���ַ�������
        std::vector<String> Split(const String& sep) const;

        // ������
        class CodePointIterator {
        private:
            const String* str;
            size_t idx;  // code point ����
        public:
            CodePointIterator(const String* s, size_t i) : str(s), idx(i) {}
            char32_t operator*() const { return str->At(idx); }
            CodePointIterator& operator++() { ++idx; return *this; }
            bool operator!=(const CodePointIterator& other) const { return idx != other.idx; }
        };

        CodePointIterator begin() const { return CodePointIterator(this, 0); }
        CodePointIterator end() const { return CodePointIterator(this, Size()); }

    private:
        std::unique_ptr<char16_t[]> m_data;  // UTF-16 ����
        size_t m_size;                        // UTF-16 ��Ԫ����
        Encoding encoding;                     // ԭʼ����
        mutable std::vector<size_t> cp_offsets; // ÿ�� Unicode code point �� UTF-16 �е�ƫ��
        mutable bool cp_cache_valid = false;   // �Ƿ񻺴���Ч

        size_t CodePointOffset(size_t index) const;

        void update_cp_cache() const;
    };
}