#include <LikesProgram/Net/Buffer.hpp>
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace LikesProgram {
    namespace Net {
        struct Buffer::BufferImpl {
            std::vector<std::uint8_t> m_buffer;    // 连续存储，包含 prepend/read/write 区域
            std::size_t m_readerIndex = 0;         // 当前可读区域起点
            std::size_t m_writerIndex = 0;         // 当前可写区域起点
        };

        Buffer::Buffer(std::size_t initialSize)
            : m_impl(new BufferImpl{}) {
            m_impl->m_buffer.resize(kCheapPrepend + initialSize);
            m_impl->m_readerIndex = kCheapPrepend;
            m_impl->m_writerIndex = kCheapPrepend;
        }

        Buffer::Buffer(const Buffer& other)
            : m_impl(new BufferImpl{}) {
            if (other.m_impl) *m_impl = *other.m_impl;
        }

        Buffer::Buffer(Buffer&& other) noexcept
            : m_impl(other.m_impl) {
            other.m_impl = nullptr;
        }

        Buffer::~Buffer() {
            delete m_impl;
            m_impl = nullptr;
        }

        Buffer& Buffer::operator=(const Buffer& other) {
            if (this == &other) return *this;

            if (!m_impl) m_impl = new BufferImpl{};
            if (other.m_impl) {
                *m_impl = *other.m_impl;
            }
            else {
                *m_impl = BufferImpl{};
            }
            return *this;
        }

        Buffer& Buffer::operator=(Buffer&& other) noexcept {
            if (this == &other) return *this;

            delete m_impl;
            m_impl = other.m_impl;
            other.m_impl = nullptr;
            return *this;
        }

        std::size_t Buffer::ReadableBytes() const noexcept {
            if (!m_impl) return 0;
            return m_impl->m_writerIndex - m_impl->m_readerIndex;
        }

        std::size_t Buffer::WritableBytes() const noexcept {
            if (!m_impl) return 0;
            return m_impl->m_buffer.size() - m_impl->m_writerIndex;
        }

        std::size_t Buffer::PrependableBytes() const noexcept {
            return m_impl ? m_impl->m_readerIndex : 0;
        }

        const std::uint8_t* Buffer::Peek() const noexcept {
            if (!m_impl) return nullptr;
            return Begin() + m_impl->m_readerIndex;
        }

        std::uint8_t* Buffer::BeginWrite() noexcept {
            if (!m_impl) return nullptr;
            return Begin() + m_impl->m_writerIndex;
        }

        const std::uint8_t* Buffer::BeginWrite() const noexcept {
            if (!m_impl) return nullptr;
            return Begin() + m_impl->m_writerIndex;
        }

        void Buffer::Consume(std::size_t len) noexcept {
            if (len < ReadableBytes()) {
                m_impl->m_readerIndex += len;
                return;
            }

            RetrieveAll();
        }

        void Buffer::RetrieveAll() noexcept {
            if (!m_impl) return;

            m_impl->m_readerIndex = kCheapPrepend;
            m_impl->m_writerIndex = kCheapPrepend;
        }

        void Buffer::TrimIfLarge() {
            if (!m_impl) m_impl = new BufferImpl{};
            if (ReadableBytes() != 0 || m_impl->m_buffer.capacity() <= kMaxIdleCapacity) return;

            std::vector<std::uint8_t> fresh; // 回收尖峰容量后的新缓冲
            fresh.resize(kCheapPrepend + kReserveAfterTrim);
            m_impl->m_buffer.swap(fresh);
            RetrieveAll();
        }

        void Buffer::Append(const void* data, std::size_t len) {
            if (data == nullptr || len == 0) return;
            Append(static_cast<const std::uint8_t*>(data), len);
        }

        void Buffer::Append(const std::uint8_t* data, std::size_t len) {
            if (data == nullptr || len == 0) return;

            EnsureWritableBytes(len);
            std::memcpy(BeginWrite(), data, len);
            HasWritten(len);
        }

        void Buffer::Append(const Buffer& other) {
            Append(other.Peek(), other.ReadableBytes());
        }

        void Buffer::HasWritten(std::size_t len) noexcept {
            const std::size_t writable = WritableBytes(); // 当前剩余可写空间
            if (m_impl) m_impl->m_writerIndex += std::min(len, writable);
        }

        void Buffer::EnsureWritableBytes(std::size_t len) {
            if (WritableBytes() < len) MakeSpace(len);
        }

        std::string_view Buffer::AsStringView() const noexcept {
            return std::string_view(
                reinterpret_cast<const char*>(Peek()),
                ReadableBytes());
        }

        std::uint8_t* Buffer::Begin() noexcept {
            return m_impl ? m_impl->m_buffer.data() : nullptr;
        }

        const std::uint8_t* Buffer::Begin() const noexcept {
            return m_impl ? m_impl->m_buffer.data() : nullptr;
        }

        void Buffer::MakeSpace(std::size_t len) {
            if (!m_impl) {
                m_impl = new BufferImpl{};
                m_impl->m_buffer.resize(kCheapPrepend);
                RetrieveAll();
            }

            if (len > std::numeric_limits<std::size_t>::max() - m_impl->m_writerIndex) {
                throw std::length_error("Buffer cannot allocate requested writable space");
            }

            if (WritableBytes() + PrependableBytes() < len + kCheapPrepend) {
                m_impl->m_buffer.resize(m_impl->m_writerIndex + len);
                return;
            }

            const std::size_t readable = ReadableBytes(); // 搬移前保存可读长度
            std::copy(Begin() + m_impl->m_readerIndex, Begin() + m_impl->m_writerIndex, Begin() + kCheapPrepend);
            m_impl->m_readerIndex = kCheapPrepend;
            m_impl->m_writerIndex = m_impl->m_readerIndex + readable;
        }
    }
}
