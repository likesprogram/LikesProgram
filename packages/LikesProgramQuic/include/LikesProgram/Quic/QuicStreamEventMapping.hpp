#pragma once

#include <LikesProgram/Quic/system/LikesProgramQuicExport.hpp>
#include <LikesProgram/Quic/QuicEngine.hpp>
#include <LikesProgram/Quic/QuicStreamFrame.hpp>
#include <LikesProgram/Core/Result.hpp>

namespace LikesProgram {
    namespace Quic {
        // Copies one parsed STREAM payload into a caller-owned event buffer.
        // A FIN event may carry the final data bytes in the same event.
        LIKESPROGRAM_QUIC_API Result<QuicStreamEvent>
            MapQuicStreamFrameToEvent(
                const QuicStreamFrame& frame,
                const Address& peer);
    }
}
