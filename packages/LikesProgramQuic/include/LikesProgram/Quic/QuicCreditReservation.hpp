#pragma once

#include <LikesProgram/Quic/system/LikesProgramQuicExport.hpp>
#include <LikesProgram/Quic/QuicConnectionCredit.hpp>
#include <LikesProgram/Quic/QuicStreamCredit.hpp>

#include <cstdint>

namespace LikesProgram {
    namespace Quic {
        enum class QuicCreditReservationError {
            None,
            ConnectionInvalidState,
            StreamInvalidState,
            ConnectionInsufficient,
            StreamInsufficient,
            ReleaseInsufficient,
        };

        struct QuicCreditReservationResult {
            QuicCreditReservationError error = QuicCreditReservationError::None;
            std::uint64_t bytes = 0;

            bool Succeeded() const noexcept {
                return error == QuicCreditReservationError::None;
            }
        };

        LIKESPROGRAM_QUIC_API QuicCreditReservationResult ReserveQuicCredit(
            QuicConnectionCreditState& connection,
            QuicStreamCreditState& stream,
            std::uint64_t bytes) noexcept;

        LIKESPROGRAM_QUIC_API QuicCreditReservationResult ReleaseQuicCredit(
            QuicConnectionCreditState& connection,
            QuicStreamCreditState& stream,
            std::uint64_t bytes) noexcept;
    }
}
