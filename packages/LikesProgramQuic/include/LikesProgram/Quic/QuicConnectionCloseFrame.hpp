#pragma once

#include <LikesProgram/Quic/system/LikesProgramQuicExport.hpp>
#include <LikesProgram/Core/Result.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace LikesProgram {
    namespace Quic {
        struct QuicConnectionCloseFrame {
            bool application = false;
            std::uint64_t errorCode = 0;
            std::uint64_t frameType = 0;
            std::span<const std::uint8_t> reason{};
            std::size_t consumedBytes = 0;
        };

        LIKESPROGRAM_QUIC_API Result<QuicConnectionCloseFrame>
            ParseQuicConnectionCloseFrame(const std::uint8_t* data, std::size_t size);
        LIKESPROGRAM_QUIC_API Result<std::vector<std::uint8_t>>
            BuildQuicConnectionCloseFrame(const QuicConnectionCloseFrame& frame);
    }
}
