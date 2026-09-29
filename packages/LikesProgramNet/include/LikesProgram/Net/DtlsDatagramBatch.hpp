#pragma once
#include <LikesProgram/Net/system/LikesProgramNetExport.hpp>
#include <LikesProgram/Net/Buffer.hpp>
#include <cstddef>

namespace LikesProgram {
    namespace Net {
        class LIKESPROGRAM_NET_API DtlsDatagramBatch {
        public:
            // 创建空的完整数据报队列。
            DtlsDatagramBatch();
            // 转移全部数据报所有权。
            DtlsDatagramBatch(DtlsDatagramBatch&& other) noexcept;
            // 释放全部自有 Buffer。
            ~DtlsDatagramBatch();

            DtlsDatagramBatch(const DtlsDatagramBatch&) = delete;
            DtlsDatagramBatch& operator=(const DtlsDatagramBatch&) = delete;

            // 移动赋值前释放当前全部数据报。
            DtlsDatagramBatch& operator=(DtlsDatagramBatch&& other) noexcept;

            // 返回当前完整数据报数量，零长度数据报也计数。
            std::size_t Count() const noexcept;
            // 返回当前全部数据报 payload 总字节数。
            std::size_t TotalBytes() const noexcept;
            // 返回当前是否没有数据报元素。
            bool Empty() const noexcept;
            // 返回指定完整数据报，越界时抛出 std::out_of_range。
            const Buffer& At(std::size_t index) const;

            // 移动接管一个完整数据报并保持其边界。
            void Append(Buffer&& datagram);
            // 移动取出队首数据报；队列为空时返回 false。
            bool TakeFront(Buffer& datagram);
            // 清空全部数据报并释放其所有权。
            void Clear() noexcept;

        private:
            struct DtlsDatagramBatchImpl;

            DtlsDatagramBatchImpl* m_impl = nullptr; // 完整数据报所有权队列
        };
    }
}
