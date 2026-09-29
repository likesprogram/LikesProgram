#pragma once

#include <LikesProgram/Quic/system/LikesProgramQuicExport.hpp>
#include <LikesProgram/Quic/QuicEngine.hpp>
#include <LikesProgram/Quic/QuicPathValidationFrame.hpp>
#include <LikesProgram/Core/Result.hpp>

namespace LikesProgram {
    namespace Quic {
        // Maps one parsed PATH_CHALLENGE to a caller-owned PATH_RESPONSE action.
        // The action id remains zero until the Engine assigns it at submission.
        LIKESPROGRAM_QUIC_API Result<QuicActionMessage>
            BuildQuicPathResponseAction(const QuicPathValidationFrame& challenge);
    }
}
