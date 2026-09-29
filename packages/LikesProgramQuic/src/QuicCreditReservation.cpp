#include <LikesProgram/Quic/QuicCreditReservation.hpp>

namespace LikesProgram {
    namespace Quic {
        QuicCreditReservationResult ReserveQuicCredit(
            QuicConnectionCreditState& connection,
            QuicStreamCreditState& stream,
            std::uint64_t bytes) noexcept {
            if (connection.reserved > connection.limit) {
                return { QuicCreditReservationError::ConnectionInvalidState, 0 };
            }
            if (stream.reserved > stream.limit) {
                return { QuicCreditReservationError::StreamInvalidState, 0 };
            }
            if (bytes > connection.Available()) {
                return { QuicCreditReservationError::ConnectionInsufficient, 0 };
            }
            if (bytes > stream.Available()) {
                return { QuicCreditReservationError::StreamInsufficient, 0 };
            }
            connection.reserved += bytes;
            stream.reserved += bytes;
            return { QuicCreditReservationError::None, bytes };
        }

        QuicCreditReservationResult ReleaseQuicCredit(
            QuicConnectionCreditState& connection,
            QuicStreamCreditState& stream,
            std::uint64_t bytes) noexcept {
            if (bytes > connection.reserved || bytes > stream.reserved) {
                return { QuicCreditReservationError::ReleaseInsufficient, 0 };
            }
            connection.reserved -= bytes;
            stream.reserved -= bytes;
            return { QuicCreditReservationError::None, bytes };
        }
    }
}
