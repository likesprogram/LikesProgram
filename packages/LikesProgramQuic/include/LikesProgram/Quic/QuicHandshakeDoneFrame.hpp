#pragma once

#include <LikesProgram/Quic/system/LikesProgramQuicExport.hpp>
#include <LikesProgram/Core/Result.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace LikesProgram {
    namespace Quic {
        struct QuicHandshakeDoneFrame {
            std::size_t consumedBytes = 0;
        };

        LIKESPROGRAM_QUIC_API Result<QuicHandshakeDoneFrame>
            ParseQuicHandshakeDoneFrame(const std::uint8_t* data, std::size_t size);

        LIKESPROGRAM_QUIC_API Result<std::vector<std::uint8_t>>
            BuildQuicHandshakeDoneFrame();
    }
}
