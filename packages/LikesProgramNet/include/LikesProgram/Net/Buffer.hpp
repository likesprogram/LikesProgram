#pragma once
#include <LikesProgram/Net/system/LikesProgramNetExport.hpp>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace LikesProgram {
    namespace Net {
        class LIKESPROGRAM_NET_API Buffer {
        public:
            static constexpr std::size_t kCheapPrepend = 8;
            static constexpr std::size_t kInitialSize = 1024;
            static constexpr std::size_t kReserveAfterTrim = 64 * 1024;
            static constexpr std::size_t kMaxIdleCapacity = 1024 * 1024;

            // 创建带预留 prepend 空间的连续缓冲。
            explicit Buffer(std::size_t initialSize = kInitialSize);
            // 复制缓冲快照。
            Buffer(const Buffer& other);
            // 移动缓冲快照。
            Buffer(Buffer&& other) noexcept;
            // 释放内部连续缓冲。
            ~Buffer();

            // 复制赋值缓冲快照。
            Buffer& operator=(const Buffer& other);
            // 移动赋值缓冲快照。
            Buffer& operator=(Buffer&& other) noexcept;

            // 返回当前可读字节数。
            std::size_t ReadableBytes() const noexcept;
            // 返回当前可写连续空间。
            std::size_t WritableBytes() const noexcept;
            // 返回 readerIndex 前可用于 prepend 的字节数。
            std::size_t PrependableBytes() const noexcept;

            // 返回可读区域起始地址。
            const std::uint8_t* Peek() const noexcept;
            // 返回可写区域起始地址。
            std::uint8_t* BeginWrite() noexcept;
            // 返回只读可写区域起始地址，便于诊断。
            const std::uint8_t* BeginWrite() const noexcept;

            // 消费 len 字节，超出时等价于清空。
            void Consume(std::size_t len) noexcept;
            // 清空可读区域，保留已分配容量。
            void RetrieveAll() noexcept;
            // 空闲容量过大时回收，避免长期连接保留尖峰内存。
            void TrimIfLarge();

            // 追加任意字节块。
            void Append(const void* data, std::size_t len);
            // 追加 uint8_t 字节块。
            void Append(const std::uint8_t* data, std::size_t len);
            // 追加另一个缓冲的可读区域。
            void Append(const Buffer& other);

            // 告知缓冲外部已经写入 len 字节。
            void HasWritten(std::size_t len) noexcept;
            // 确保至少 len 字节连续可写空间。
            void EnsureWritableBytes(std::size_t len);
            // 返回可读区域的 string_view，适合文本协议零拷贝解析。
            std::string_view AsStringView() const noexcept;

        private:
            struct BufferImpl;

            // 返回底层数组起点。
            std::uint8_t* Begin() noexcept;
            // 返回底层数组起点的只读版本。
            const std::uint8_t* Begin() const noexcept;
            // 移动或扩容，为追加写入腾出空间。
            void MakeSpace(std::size_t len);

            BufferImpl* m_impl = nullptr;          // 缓冲实现，避免导出类携带 std::vector
        };
    }
}
