#pragma once

#include <LikesProgram/Quic/system/LikesProgramQuicExport.hpp>
#include <LikesProgram/Core/Result.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace LikesProgram {
    namespace Quic {
        enum class QuicConnectionIdFrameKind {
            NewConnectionId,
            RetireConnectionId,
        };

        struct QuicConnectionIdFrame {
            QuicConnectionIdFrameKind kind = QuicConnectionIdFrameKind::NewConnectionId;
            std::uint64_t sequenceNumber = 0;
            std::uint64_t retirePriorTo = 0;
            std::span<const std::uint8_t> connectionId{};
            std::span<const std::uint8_t> statelessResetToken{};
            std::size_t consumedBytes = 0;
        };

        // Parses one NEW_CONNECTION_ID or RETIRE_CONNECTION_ID frame. Returned
        // spans borrow the input buffer and remain valid only while it lives.
        LIKESPROGRAM_QUIC_API Result<QuicConnectionIdFrame>
            ParseQuicConnectionIdFrame(const std::uint8_t* data, std::size_t size);

        // Builds one connection-id frame without owning connection migration
        // policy, reset-token rotation, or path state.
        LIKESPROGRAM_QUIC_API Result<std::vector<std::uint8_t>>
            BuildQuicConnectionIdFrame(const QuicConnectionIdFrame& frame);
    }
}
