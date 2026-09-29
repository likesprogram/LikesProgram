#pragma once

#include <LikesProgram/Quic/system/LikesProgramQuicExport.hpp>
#include <LikesProgram/Core/Result.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace LikesProgram {
    namespace Quic {
        struct QuicNewTokenFrame {
            std::span<const std::uint8_t> token{};
            std::size_t consumedBytes = 0;
        };

        struct QuicNewTokenFrameBuildOptions {
            std::span<const std::uint8_t> token{};
        };

        // Parses one NEW_TOKEN frame and returns a non-owning token span.
        LIKESPROGRAM_QUIC_API Result<QuicNewTokenFrame>
            ParseQuicNewTokenFrame(const std::uint8_t* data, std::size_t size);

        // Builds one NEW_TOKEN frame; token validation/cryptography stays with the caller.
        LIKESPROGRAM_QUIC_API Result<std::vector<std::uint8_t>>
            BuildQuicNewTokenFrame(const QuicNewTokenFrameBuildOptions& options);
    }
}
