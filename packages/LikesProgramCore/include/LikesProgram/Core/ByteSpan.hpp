#pragma once
#include <LikesProgram/Core/String.hpp>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>

namespace LikesProgram {
    // 只读字节视图，不拥有底层内存。
    class ConstByteSpan {
    public:
        static constexpr size_t npos = static_cast<size_t>(-1); // 表示一直截取到末尾

        // 构造空字节视图。
        ConstByteSpan() = default;

        // 从任意原始内存构造只读视图。
        ConstByteSpan(const void* data, size_t size) noexcept
            : m_data(static_cast<const std::byte*>(data)), m_size(data ? size : 0) { }

        // 从标准 span 构造只读视图。
        ConstByteSpan(std::span<const std::byte> span) noexcept
            : m_data(span.data()), m_size(span.size()) { }

        // 返回字节指针。
        const std::byte* Data() const noexcept {
            return m_data;
        }

        // 返回便于传统 API 使用的 uint8_t 指针。
        const uint8_t* Bytes() const noexcept {
            return reinterpret_cast<const uint8_t*>(m_data);
        }

        // 返回字节数。
        size_t Size() const noexcept {
            return m_size;
        }

        // 判断视图是否为空。
        bool Empty() const noexcept {
            return m_size == 0;
        }

        // 创建子视图，越界时抛出异常。
        ConstByteSpan SubSpan(size_t offset, size_t count = npos) const {
            if (offset > m_size) throw std::out_of_range("ByteSpan offset out of range");
            size_t actualCount = count == npos ? m_size - offset : count; // 实际截取字节数
            if (actualCount > m_size - offset) throw std::out_of_range("ByteSpan count out of range");
            const std::byte* begin = m_data ? m_data + offset : nullptr; // 空视图不做空指针算术
            return ConstByteSpan(begin, actualCount);
        }

        // 按字节访问，越界时抛出异常。
        std::byte At(size_t index) const {
            if (index >= m_size) throw std::out_of_range("ByteSpan index out of range");
            return m_data[index];
        }

        // 按字节访问，调用方保证不越界。
        std::byte operator[](size_t index) const noexcept {
            return m_data[index];
        }

        // 转换为十六进制文本。
        String ToHexString(bool upper = false) const {
            const char16_t* digits = upper ? u"0123456789ABCDEF" : u"0123456789abcdef"; // 十六进制字符表
            String text; // 输出文本，每个字节生成两个字符
            for (size_t i = 0; i < m_size; ++i) {
                auto value = std::to_integer<unsigned int>(m_data[i]); // 当前字节无符号值
                text.Append(digits[(value >> 4) & 0x0F]);
                text.Append(digits[value & 0x0F]);
            }
            return text;
        }

    private:
        const std::byte* m_data = nullptr; // 不拥有的只读字节缓冲区
        size_t m_size = 0;                 // 字节数
    };

    // 可写字节视图，不拥有底层内存。
    class ByteSpan {
    public:
        static constexpr size_t npos = ConstByteSpan::npos; // 表示一直截取到末尾

        // 构造空可写视图。
        ByteSpan() = default;

        // 从任意原始内存构造可写视图。
        ByteSpan(void* data, size_t size) noexcept
            : m_data(static_cast<std::byte*>(data)), m_size(data ? size : 0) { }

        // 从标准 span 构造可写视图。
        ByteSpan(std::span<std::byte> span) noexcept
            : m_data(span.data()), m_size(span.size()) { }

        // 返回可写字节指针。
        std::byte* Data() const noexcept {
            return m_data;
        }

        // 返回便于传统 API 使用的 uint8_t 指针。
        uint8_t* Bytes() const noexcept {
            return reinterpret_cast<uint8_t*>(m_data);
        }

        // 返回字节数。
        size_t Size() const noexcept {
            return m_size;
        }

        // 判断视图是否为空。
        bool Empty() const noexcept {
            return m_size == 0;
        }

        // 转换为只读字节视图。
        ConstByteSpan AsConst() const noexcept {
            return ConstByteSpan(m_data, m_size);
        }

        // 创建可写子视图，越界时抛出异常。
        ByteSpan SubSpan(size_t offset, size_t count = npos) const {
            if (offset > m_size) throw std::out_of_range("ByteSpan offset out of range");
            size_t actualCount = count == npos ? m_size - offset : count; // 实际截取字节数
            if (actualCount > m_size - offset) throw std::out_of_range("ByteSpan count out of range");
            std::byte* begin = m_data ? m_data + offset : nullptr; // 空视图不做空指针算术
            return ByteSpan(begin, actualCount);
        }

        // 使用同一个字节填充整段视图。
        void Fill(std::byte value) const {
            if (!m_data || m_size == 0) return;
            std::fill(m_data, m_data + m_size, value);
        }

        // 按字节访问，越界时抛出异常。
        std::byte& At(size_t index) const {
            if (index >= m_size) throw std::out_of_range("ByteSpan index out of range");
            return m_data[index];
        }

        // 按字节访问，调用方保证不越界。
        std::byte& operator[](size_t index) const noexcept {
            return m_data[index];
        }

    private:
        std::byte* m_data = nullptr; // 不拥有的可写字节缓冲区
        size_t m_size = 0;           // 字节数
    };
}
