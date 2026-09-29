#pragma once
#include <LikesProgram/Net/system/LikesProgramNetExport.hpp>
#include <cstddef>
#include <cstdint>

namespace LikesProgram {
    namespace Net {
        namespace Internal {
            struct BufferLeaseAccess;
        }

        class LIKESPROGRAM_NET_API BufferLease {
        public:
            // 创建空 lease，不持有外部 buffer。
            BufferLease() noexcept;
            // 移动 provided-buffer 所有权。
            BufferLease(BufferLease&& other) noexcept;
            // 释放 lease 并把 buffer 归还原始池。
            ~BufferLease();

            BufferLease(const BufferLease&) = delete;
            BufferLease& operator=(const BufferLease&) = delete;

            // 移动赋值前先归还当前 buffer。
            BufferLease& operator=(BufferLease&& other) noexcept;

            // 返回 lease 字节起点。
            const std::uint8_t* Data() const noexcept;
            // 返回 lease 字节数。
            std::size_t Size() const noexcept;
            // 返回当前是否为空 lease。
            bool Empty() const noexcept;
            // 立即归还 buffer，并把 lease 置空。
            void Reset() noexcept;

        private:
            friend struct Internal::BufferLeaseAccess;
            using ReleaseFunction = void(*)(void*, std::uint32_t) noexcept;

            // 仅平台 buffer 池可创建带归还回调的 lease。
            BufferLease(
                std::uint8_t* data,
                std::size_t len,
                void* owner,
                std::uint32_t token,
                ReleaseFunction releaseFunction) noexcept;

            std::uint8_t* m_data = nullptr;              // 平台池提供的稳定字节起点
            std::size_t m_len = 0;                       // 当前 lease 可见字节数
            void* m_owner = nullptr;                     // buffer 池上下文，不拥有生命周期
            std::uint32_t m_token = 0;                   // 平台池内部 buffer id
            ReleaseFunction m_releaseFunction = nullptr; // 无异常归还入口
        };
    }
}
