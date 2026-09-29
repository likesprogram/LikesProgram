#pragma once

#include <LikesProgram/Quic/system/LikesProgramQuicExport.hpp>
#include <LikesProgram/Core/Result.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace LikesProgram {
    namespace Quic {
        struct QuicStreamFrame {
            std::uint64_t streamId = 0;
            std::uint64_t offset = 0;
            bool hasOffset = false;
            bool hasLength = false;
            bool fin = false;
            std::span<const std::uint8_t> data{};
            std::size_t consumedBytes = 0;
        };

        struct QuicStreamFrameBuildOptions {
            std::uint64_t streamId = 0;
            std::uint64_t offset = 0;
            bool hasOffset = false;
            bool hasLength = true;
            bool fin = false;
            std::span<const std::uint8_t> data{};
        };

        // Parses one STREAM frame and returns a non-owning payload span.
        // Without LEN, the frame consumes all remaining input bytes.
        LIKESPROGRAM_QUIC_API Result<QuicStreamFrame> ParseQuicStreamFrame(
            const std::uint8_t* data,
            std::size_t size);

        // Builds one STREAM frame. Stream scheduling and flow control remain
        // outside this wire-format helper.
        LIKESPROGRAM_QUIC_API Result<std::vector<std::uint8_t>>
            BuildQuicStreamFrame(const QuicStreamFrameBuildOptions& options);
    }
}
