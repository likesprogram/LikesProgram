#pragma once

#include <LikesProgram/Quic/system/LikesProgramQuicExport.hpp>
#include <LikesProgram/Core/Result.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace LikesProgram {
    namespace Quic {
        enum class QuicFlowControlFrameKind {
            MaxData,
            MaxStreamData,
            MaxStreamsBidi,
            MaxStreamsUni,
            DataBlocked,
            StreamDataBlocked,
            StreamsBlockedBidi,
            StreamsBlockedUni,
        };

        struct QuicFlowControlFrame {
            QuicFlowControlFrameKind kind = QuicFlowControlFrameKind::MaxData;
            std::uint64_t streamId = 0;
            std::uint64_t limit = 0;
            std::size_t consumedBytes = 0;

            friend bool operator==(const QuicFlowControlFrame&,
                const QuicFlowControlFrame&) = default;
        };

        // Parses one QUIC flow-control frame and leaves trailing input untouched.
        LIKESPROGRAM_QUIC_API Result<QuicFlowControlFrame>
            ParseQuicFlowControlFrame(const std::uint8_t* data, std::size_t size);

        // Builds one QUIC flow-control frame. Connection and stream accounting
        // remain outside this wire-format helper.
        LIKESPROGRAM_QUIC_API Result<std::vector<std::uint8_t>>
            BuildQuicFlowControlFrame(const QuicFlowControlFrame& frame);
    }
}
