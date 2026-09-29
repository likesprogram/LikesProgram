#pragma once
#include <LikesProgram/Net/system/LikesProgramNetExport.hpp>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace LikesProgram {
    namespace Net {
        class LIKESPROGRAM_NET_API BufferSlice {
        public:
            // 创建空借用视图。
            BufferSlice() noexcept;
            // 借用一段连续只读字节；owner 变更或销毁后视图立即失效。
            BufferSlice(const std::uint8_t* data, std::size_t len) noexcept;

            // 返回借用字节起点。
            const std::uint8_t* Data() const noexcept;
            // 返回借用字节数。
            std::size_t Size() const noexcept;
            // 返回当前视图是否为空。
            bool Empty() const noexcept;
            // 返回只读文本视图；生命周期与当前 slice 相同。
            std::string_view AsStringView() const noexcept;

        private:
            const std::uint8_t* m_data = nullptr; // 由 Buffer 或 BufferChain 保持有效的借用起点
            std::size_t m_len = 0;                // 当前借用范围字节数
        };
    }
}
