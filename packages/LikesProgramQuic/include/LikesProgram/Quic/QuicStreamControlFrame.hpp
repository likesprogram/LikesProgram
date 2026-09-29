#pragma once

#include <LikesProgram/Quic/system/LikesProgramQuicExport.hpp>
#include <LikesProgram/Core/Result.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace LikesProgram {
    namespace Quic {
        struct QuicResetStreamFrame {
            std::uint64_t streamId = 0;
            std::uint64_t applicationErrorCode = 0;
            std::uint64_t finalSize = 0;
            std::size_t consumedBytes = 0;
        };

        struct QuicStopSendingFrame {
            std::uint64_t streamId = 0;
            std::uint64_t applicationErrorCode = 0;
            std::size_t consumedBytes = 0;
        };

        LIKESPROGRAM_QUIC_API Result<QuicResetStreamFrame>
            ParseQuicResetStreamFrame(const std::uint8_t* data, std::size_t size);
        LIKESPROGRAM_QUIC_API Result<std::vector<std::uint8_t>>
            BuildQuicResetStreamFrame(const QuicResetStreamFrame& frame);

        LIKESPROGRAM_QUIC_API Result<QuicStopSendingFrame>
            ParseQuicStopSendingFrame(const std::uint8_t* data, std::size_t size);
        LIKESPROGRAM_QUIC_API Result<std::vector<std::uint8_t>>
            BuildQuicStopSendingFrame(const QuicStopSendingFrame& frame);
    }
}
