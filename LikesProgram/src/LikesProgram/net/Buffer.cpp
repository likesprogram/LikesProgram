#include "../../../include/LikesProgram/net/Buffer.hpp"

namespace LikesProgram {
	namespace Net {
		Buffer::Buffer(size_t initialSize)
			: m_buffer(kCheapPrepend + initialSize), m_readerIndex(kCheapPrepend), m_writerIndex(kCheapPrepend) {
        }

		size_t Buffer::ReadableBytes() const noexcept {
			return m_writerIndex - m_readerIndex;
		}

		size_t Buffer::WritableBytes() const noexcept {
			return m_buffer.size() - m_writerIndex;
		}

		size_t Buffer::PrependableBytes() const noexcept {
			return m_readerIndex;
		}

        const uint8_t* Buffer::Peek() const noexcept {
            return m_buffer.data() + m_readerIndex;
        }

        uint8_t* Buffer::BeginWrite() noexcept {
            return m_buffer.data() + m_writerIndex;
        }

        const uint8_t* Buffer::BeginWrite() const noexcept {
            return m_buffer.data() + m_writerIndex;
        }

        // 读：消费
        void Buffer::Consume(size_t len) noexcept {
            if (len >= ReadableBytes()) {
                RetrieveAll();
            } else {
                m_readerIndex += len;
            }
        }

        void Buffer::RetrieveAll() noexcept {
            // 重置 指针位置
            m_readerIndex = kCheapPrepend;
            m_writerIndex = kCheapPrepend;
            // 重置大小
            TrimIfLarge();
        }

        void Buffer::TrimIfLarge() noexcept {
            // 只有在“空”且“容量过大”时才缩
            if (ReadableBytes() != 0) return;
            if (m_buffer.capacity() <= kMaxIdleCapacity) return;

            try {
                std::vector<uint8_t> newbuf;
                newbuf.resize(kCheapPrepend + kReserveAfterTrim);
                m_buffer.swap(newbuf);
                m_readerIndex = kCheapPrepend;
                m_writerIndex = kCheapPrepend;
            }
            catch (...) { /* 放弃 shrink，保证不崩 */ }
        }

        // 写：追加
        void Buffer::Append(const void* data, size_t len) {
            if (!data || len == 0) return;
            EnsureWritableBytes(len);
            std::memcpy(BeginWrite(), data, len);
            HasWritten(len);
        }

        void Buffer::Append(const uint8_t* data, size_t len) {
            Append((const void*)data, len);
        }

        void Buffer::Append(const Buffer& other) {
            Append(other.Peek(), other.ReadableBytes());
        }

        // 写入后推进 writer
        void Buffer::HasWritten(size_t len) noexcept {
            m_writerIndex += len;
        }

        // 确保可写空间
        void Buffer::EnsureWritableBytes(size_t len) {
            if (WritableBytes() < len) {
                MakeSpace(len);
            }
        }

        // 给协议层提供 view（零拷贝读）
        std::string_view Buffer::AsStringView() const noexcept {
            return std::string_view(reinterpret_cast<const char*>(Peek()), ReadableBytes());
        }

        uint8_t* Buffer::Begin() noexcept {
            return m_buffer.data();
        }

        const uint8_t* Buffer::Begin() const noexcept {
            return m_buffer.data();
        }

        void Buffer::MakeSpace(size_t len) {
            // 策略：优先“整理”（把可读数据搬到前面），再扩容
            // 可整理的空间 = prependable + writable
            if (PrependableBytes() + WritableBytes() < len + kCheapPrepend) {
                // 不够，扩容
                m_buffer.resize(std::max(m_buffer.size() * 2, m_writerIndex + len));
            }
            else {
                // 整理：把可读数据挪到 kCheapPrepend 开始处
                const size_t readable = ReadableBytes();
                std::memmove(Begin() + kCheapPrepend, Begin() + m_readerIndex, readable);
                m_readerIndex = kCheapPrepend;
                m_writerIndex = m_readerIndex + readable;
            }
        }
	}
}
