#include <LikesProgram/Quic/QuicTimeout.hpp>

namespace LikesProgram {
    namespace Quic {
        QuicTimeoutSnapshot EvaluateQuicTimeout(
            const QuicTimeoutContext& context,
            Time::SteadyTimePoint now) noexcept {
            if (context.closing.HasDeadline() && now >= context.closing.TimePoint()) {
                return { QuicTimeoutReason::ClosingDeadline,
                    QuicTimeoutAction::FinishClosing };
            }
            if (context.handshake.HasDeadline() && now >= context.handshake.TimePoint()) {
                return { QuicTimeoutReason::HandshakeDeadline,
                    QuicTimeoutAction::FailHandshake };
            }
            if (context.idle.HasDeadline() && now >= context.idle.TimePoint()) {
                return { QuicTimeoutReason::IdleDeadline,
                    QuicTimeoutAction::CloseIdle };
            }
            return {};
        }
    }
}
