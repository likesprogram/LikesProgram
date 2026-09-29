#pragma once

#include <LikesProgram/Quic/system/LikesProgramQuicExport.hpp>
#include <LikesProgram/Quic/QuicEngine.hpp>
#include <LikesProgram/Quic/QuicStreamControlFrame.hpp>
#include <LikesProgram/Core/Result.hpp>

namespace LikesProgram {
    namespace Quic {
        LIKESPROGRAM_QUIC_API Result<QuicActionMessage>
            BuildQuicResetStreamAction(const QuicResetStreamFrame& frame);

        LIKESPROGRAM_QUIC_API Result<QuicActionMessage>
            BuildQuicStopSendingAction(const QuicStopSendingFrame& frame);
    }
}
