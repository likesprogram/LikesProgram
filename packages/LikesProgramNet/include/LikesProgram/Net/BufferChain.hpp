#pragma once
#include <LikesProgram/Net/system/LikesProgramNetExport.hpp>
#include <LikesProgram/Net/Buffer.hpp>
#include <LikesProgram/Net/BufferSlice.hpp>
#include <cstddef>

namespace LikesProgram {
    namespace Net {
        class LIKESPROGRAM_NET_API BufferChain {
        public:
            // 创建空的多段拥有链。
            BufferChain();
            // 转移全部段所有权。
            BufferChain(BufferChain&& other) noexcept;
            // 释放全部自有 Buffer 与 provided-buffer lease。
            ~BufferChain();

            BufferChain(const BufferChain&) = delete;
            BufferChain& operator=(const BufferChain&) = delete;

            // 移动赋值前先释放当前全部段。
            BufferChain& operator=(BufferChain&& other) noexcept;

            // 返回全部未消费段的总字节数。
            std::size_t ReadableBytes() const noexcept;
            // 返回当前未消费段数量。
            std::size_t SegmentCount() const noexcept;
            // 返回链是否没有可读字节。
            bool Empty() const noexcept;
            // 返回指定未消费段的借用视图，越界时返回空视图。
            BufferSlice Segment(std::size_t index) const noexcept;

            // 接管一个 Buffer 段的可读区域。
            void Append(Buffer&& buffer);
            // 将 provided-buffer lease 作为独立零复制段接入链尾。
            void Append(BufferLease&& lease);
            // 移动接管另一条链的全部未消费段，不复制 payload。
            void Append(BufferChain&& chain);
            // 跨段消费 len 字节，已清空段立即释放其 lease。
            void Consume(std::size_t len) noexcept;
            // 将全部未消费 lease 段复制到自有存储。
            void Materialize();
            // 清空全部段并释放所有权。
            void Clear() noexcept;

        private:
            struct BufferChainImpl;

            BufferChainImpl* m_impl = nullptr; // 隐藏 vector 与消费游标，保持公共 ABI 稳定
        };
    }
}
