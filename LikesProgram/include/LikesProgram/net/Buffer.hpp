#pragma once
#include <vector>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <string_view>

namespace LikesProgram {
    namespace Net {
        class Buffer {
        public:
            static constexpr size_t kCheapPrepend = 8;
            static constexpr size_t kInitialSize = 1024;

            explicit Buffer(size_t initialSize = kInitialSize);

            // 基本信息
            size_t ReadableBytes() const noexcept;
            size_t WritableBytes() const noexcept;
            size_t PrependableBytes() const noexcept;

            const uint8_t* Peek() const noexcept;
            uint8_t* BeginWrite() noexcept;
            const uint8_t* BeginWrite() const noexcept;

            size_t Size() const noexcept;
            const uint8_t* Data() const noexcept;

            // 读：消费
            void Consume(size_t len) noexcept;

            void RetrieveAll() noexcept;

            // 写：追加
            void Append(const void* data, size_t len);

            void Append(const uint8_t* data, size_t len);

            void Append(const Buffer& other);

            // 写入后推进 writer
            void HasWritten(size_t len) noexcept;

            // 确保可写空间
            void EnsureWritableBytes(size_t len);

            // 给协议层提供 view（零拷贝读）
            std::string_view AsStringView() const noexcept;

        private:
            uint8_t* Begin() noexcept;
            const uint8_t* Begin() const noexcept;

            void MakeSpace(size_t len);

        private:
            std::vector<uint8_t> m_buffer;
            size_t m_readerIndex = 0;
            size_t m_writerIndex = 0;
        };
    }
}
