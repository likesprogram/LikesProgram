#pragma once

#include <LikesProgram/Quic/system/LikesProgramQuicExport.hpp>
#include <LikesProgram/Core/Result.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace LikesProgram {
    namespace Quic {
        enum class QuicPathValidationFrameKind {
            PathChallenge,
            PathResponse,
        };

        struct QuicPathValidationFrame {
            QuicPathValidationFrameKind kind = QuicPathValidationFrameKind::PathChallenge;
            std::array<std::uint8_t, 8> data{};
            std::size_t consumedBytes = 0;

            friend bool operator==(const QuicPathValidationFrame&,
                const QuicPathValidationFrame&) = default;
        };

        // Parses one PATH_CHALLENGE or PATH_RESPONSE frame.
        LIKESPROGRAM_QUIC_API Result<QuicPathValidationFrame>
            ParseQuicPathValidationFrame(const std::uint8_t* data, std::size_t size);

        // Builds one path-validation frame. Path probing and migration state
        // remain outside this wire-format helper.
        LIKESPROGRAM_QUIC_API Result<std::vector<std::uint8_t>>
            BuildQuicPathValidationFrame(const QuicPathValidationFrame& frame);

        // Echoes an accepted PATH_CHALLENGE token as a PATH_RESPONSE frame.
        // The helper does not retain the token or validate a peer path.
        LIKESPROGRAM_QUIC_API Result<QuicPathValidationFrame>
            BuildQuicPathResponseFrame(const QuicPathValidationFrame& challenge);
    }
}
