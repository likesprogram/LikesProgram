#pragma once

#include <LikesProgram/Quic/system/LikesProgramQuicExport.hpp>
#include <LikesProgram/Quic/QuicConnectionCloseFrame.hpp>
#include <LikesProgram/Quic/QuicEngine.hpp>
#include <LikesProgram/Core/Result.hpp>

namespace LikesProgram {
    namespace Quic {
        LIKESPROGRAM_QUIC_API Result<QuicStreamEvent>
            MapQuicConnectionCloseFrameToEvent(
                const QuicConnectionCloseFrame& frame,
                const Address& peer);
    }
}
