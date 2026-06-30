#pragma once
#include <LikesProgram/Core/String.hpp>
#include <stdexcept>
#include <string_view>

namespace LikesProgram {
    // UTF-16 只读字符串视图，不拥有底层内存。
    class StringView {
    public:
        // 构造空视图。
        StringView() = default;

        // 从 String 构造视图；调用方必须保证 String 生命周期覆盖视图。
        StringView(const String& text) noexcept
            : m_data(text.data()), m_length(text.Length()) { }

        // 从 NUL 结尾 UTF-16 文本构造视图。
        StringView(const char16_t* text) noexcept
            : m_data(text), m_length(CountLength(text)) { }

        // 从 UTF-16 指针和长度构造视图。
        StringView(const char16_t* text, size_t length) noexcept
            : m_data(text), m_length(text ? length : 0) { }

        // 从标准 UTF-16 视图构造。
        StringView(std::u16string_view view) noexcept
            : m_data(view.data()), m_length(view.size()) { }

        // 返回 UTF-16 code unit 数。
        size_t Length() const noexcept {
            return m_length;
        }

        // 判断视图是否为空。
        bool Empty() const noexcept {
            return m_length == 0;
        }

        // 返回底层 UTF-16 指针，空视图可为 nullptr。
        const char16_t* Data() const noexcept {
            return m_data;
        }

        // 返回标准 UTF-16 视图。
        std::u16string_view ToStdView() const noexcept {
            return std::u16string_view(m_data ? m_data : u"", m_length);
        }

        // 转换为拥有型 String。
        String ToString() const {
            return String(ToStdView());
        }

        // 按 UTF-16 code unit 访问，越界时抛出异常。
        char16_t At(size_t index) const {
            if (index >= m_length) throw std::out_of_range("StringView index out of range");
            return m_data[index];
        }

        // 按 UTF-16 code unit 访问，调用方保证不越界。
        char16_t operator[](size_t index) const noexcept {
            return m_data[index];
        }

    private:
        // 计算 NUL 结尾 UTF-16 字符串长度。
        static size_t CountLength(const char16_t* text) noexcept {
            if (!text) return 0;
            size_t length = 0; // 已扫描的 UTF-16 code unit 数
            while (text[length] != u'\0') ++length;
            return length;
        }

        const char16_t* m_data = nullptr; // 不拥有的 UTF-16 缓冲区
        size_t m_length = 0;              // 视图长度，单位为 UTF-16 code unit
    };
}
