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
            // 预留在 buffer 前部的空间
            static constexpr size_t kCheapPrepend = 8;
            // 默认初始容量（不含 prepend）
            static constexpr size_t kInitialSize = 1024;
            // trim 后保留的容量（不含 prepend）
            static constexpr size_t kReserveAfterTrim = 64 * 1024;
            // 触发 trim 的阈值（按 vector.capacity()）
            static constexpr size_t kMaxIdleCapacity = 1024 * 1024;
            // 构造函数：初始化 buffer 大小
            explicit Buffer(size_t initialSize = kInitialSize);

            // 当前可读数据大小（协议层最常用）
            size_t ReadableBytes() const noexcept;
            // 当前可写空间大小
            size_t WritableBytes() const noexcept;
            // readerIndex 前可用空间, 用于 prepend 操作
            size_t PrependableBytes() const noexcept;

            // 指向当前可读数据起始位置
            const uint8_t* Peek() const noexcept;
            // 指向当前可写位置
            uint8_t* BeginWrite() noexcept;
            // const 版本的写指针（只读场景）
            const uint8_t* BeginWrite() const noexcept;

            // 消费 len 字节（推进 readerIndex）
            void Consume(size_t len) noexcept;

            // 清空所有可读数据
            void RetrieveAll() noexcept;

            // 重置内存
            void TrimIfLarge() noexcept;

            // 写：追加
            void Append(const void* data, size_t len);
            // 写：追加
            void Append(const uint8_t* data, size_t len);
            // 写：追加
            void Append(const Buffer& other);

            // 写入后推进 writer
            void HasWritten(size_t len) noexcept;

            // 确保至少有 len 字节的可写空间
            void EnsureWritableBytes(size_t len);

            // 给协议层提供 view（零拷贝读）
            std::string_view AsStringView() const noexcept;

        private:
            // 返回 buffer 起始地址（内部使用）
            uint8_t* Begin() noexcept;
            const uint8_t* Begin() const noexcept;
            // 为写入腾出空间
            void MakeSpace(size_t len);

        private:
            std::vector<uint8_t> m_buffer;
            size_t m_readerIndex = 0;
            size_t m_writerIndex = 0;
        };
    }
}
