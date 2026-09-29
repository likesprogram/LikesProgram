#include <LikesProgram/Net/Buffer.hpp>
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace LikesProgram {
    namespace Net {
        struct Buffer::BufferImpl {
            std::vector<std::uint8_t> m_buffer;    // 连续存储，包含 prepend/read/write 区域
            std::size_t m_readerIndex = 0;         // 当前可读区域起点
            std::size_t m_writerIndex = 0;         // 当前可写区域起点
            BufferLease m_lease;                   // 单段 provided-buffer 零复制所有权
            std::size_t m_leaseOffset = 0;          // lease 已消费字节偏移
        };

        Buffer::Buffer(std::size_t initialSize)
            : m_impl(new BufferImpl{}) {
            m_impl->m_buffer.resize(kCheapPrepend + initialSize);
            m_impl->m_readerIndex = kCheapPrepend;
            m_impl->m_writerIndex = kCheapPrepend;
        }

        Buffer::Buffer(const Buffer& other)
            : m_impl(new BufferImpl{}) {
            CopyFrom(other);
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
            m_impl->m_lease.Reset();
            m_impl->m_leaseOffset = 0;
            CopyFrom(other);
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
            if (!m_impl->m_lease.Empty()) return m_impl->m_lease.Size() - m_impl->m_leaseOffset;
            return m_impl->m_writerIndex - m_impl->m_readerIndex;
        }

        std::size_t Buffer::WritableBytes() const noexcept {
            if (!m_impl) return 0;
            if (!m_impl->m_lease.Empty()) return 0;
            return m_impl->m_buffer.size() - m_impl->m_writerIndex;
        }

        std::size_t Buffer::PrependableBytes() const noexcept {
            if (m_impl != nullptr && !m_impl->m_lease.Empty()) return 0;
            return m_impl ? m_impl->m_readerIndex : 0;
        }

        const std::uint8_t* Buffer::Peek() const noexcept {
            if (!m_impl) return nullptr;
            if (!m_impl->m_lease.Empty()) return m_impl->m_lease.Data() + m_impl->m_leaseOffset;
            return Begin() + m_impl->m_readerIndex;
        }

        std::uint8_t* Buffer::BeginWrite() noexcept {
            if (!m_impl || !m_impl->m_lease.Empty()) return nullptr;
            return Begin() + m_impl->m_writerIndex;
        }

        const std::uint8_t* Buffer::BeginWrite() const noexcept {
            if (!m_impl || !m_impl->m_lease.Empty()) return nullptr;
            return Begin() + m_impl->m_writerIndex;
        }

        std::uint8_t* Buffer::PrepareWrite(std::size_t len) {
            EnsureWritableBytes(len);
            return BeginWrite();
        }

        void Buffer::Consume(std::size_t len) noexcept {
            if (m_impl != nullptr && !m_impl->m_lease.Empty()) {
                const std::size_t readable = ReadableBytes(); // 当前 lease 未消费字节数
                if (len < readable) {
                    m_impl->m_leaseOffset += len;
                    return;
                }
                RetrieveAll();
                return;
            }

            if (len < ReadableBytes()) {
                m_impl->m_readerIndex += len;
                return;
            }

            RetrieveAll();
        }

        void Buffer::RetrieveAll() noexcept {
            if (!m_impl) return;

            m_impl->m_lease.Reset();
            m_impl->m_leaseOffset = 0;
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

        void Buffer::Materialize() {
            // completion 回调外继续保留数据时，显式脱离平台 provided-buffer 池。
            MaterializeLease();
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

        void Buffer::Append(BufferLease&& lease) {
            if (lease.Empty()) return;
            if (!m_impl) m_impl = new BufferImpl{};

            if (ReadableBytes() == 0) {
                // 空 Buffer 直接接管 lease，Peek/AsStringView 保持原 provided-buffer 地址。
                RetrieveAll();
                m_impl->m_lease = std::move(lease);
                return;
            }

            // 已有未消费数据时必须保持 Buffer 的单段连续契约，只在该边界物化复制。
            MaterializeLease();
            Append(lease.Data(), lease.Size());
            lease.Reset();
        }

        void Buffer::HasWritten(std::size_t len) noexcept {
            const std::size_t writable = WritableBytes(); // 当前剩余可写空间
            if (m_impl) m_impl->m_writerIndex += std::min(len, writable);
        }

        void Buffer::EnsureWritableBytes(std::size_t len) {
            MaterializeLease();
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

        void Buffer::MaterializeLease() {
            if (m_impl == nullptr || m_impl->m_lease.Empty()) return;

            const std::uint8_t* data = Peek(); // Reset 前保存 lease 未消费区
            const std::size_t len = ReadableBytes(); // 需要转入自有存储的字节数
            std::vector<std::uint8_t> materialized; // 先完成可能抛异常的分配，再归还 lease
            materialized.resize(kCheapPrepend + len);
            if (len > 0) std::memcpy(materialized.data() + kCheapPrepend, data, len);

            m_impl->m_buffer.swap(materialized);
            m_impl->m_readerIndex = kCheapPrepend;
            m_impl->m_writerIndex = kCheapPrepend + len;
            m_impl->m_lease.Reset();
            m_impl->m_leaseOffset = 0;
        }

        void Buffer::CopyFrom(const Buffer& other) {
            if (!m_impl) return;

            const std::size_t len = other.ReadableBytes(); // 复制只保留可读字节值语义
            m_impl->m_buffer.resize(kCheapPrepend + len);
            m_impl->m_readerIndex = kCheapPrepend;
            m_impl->m_writerIndex = kCheapPrepend + len;
            if (len > 0) std::memcpy(m_impl->m_buffer.data() + kCheapPrepend, other.Peek(), len);
        }
    }
}
