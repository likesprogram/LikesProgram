#pragma once

#include <LikesProgram/Quic/system/LikesProgramQuicExport.hpp>
#include <LikesProgram/Core/time/Deadline.hpp>

namespace LikesProgram {
    namespace Quic {
        enum class QuicTimeoutReason {
            None,
            HandshakeDeadline,
            IdleDeadline,
            ClosingDeadline,
        };

        enum class QuicTimeoutAction {
            None,
            FailHandshake,
            CloseIdle,
            FinishClosing,
        };

        struct QuicTimeoutContext {
            Time::Deadline handshake = Time::Deadline::Infinite();
            Time::Deadline idle = Time::Deadline::Infinite();
            Time::Deadline closing = Time::Deadline::Infinite();

            void Reset() noexcept {
                handshake = Time::Deadline::Infinite();
                idle = Time::Deadline::Infinite();
                closing = Time::Deadline::Infinite();
            }
        };

        struct QuicTimeoutSnapshot {
            QuicTimeoutReason reason = QuicTimeoutReason::None;
            QuicTimeoutAction action = QuicTimeoutAction::None;

            bool Expired() const noexcept {
                return reason != QuicTimeoutReason::None;
            }
        };

        // Evaluates caller-provided deadlines against caller-provided time.
        // No clock read, timer ownership or Engine mutation occurs here.
        LIKESPROGRAM_QUIC_API QuicTimeoutSnapshot EvaluateQuicTimeout(
            const QuicTimeoutContext& context,
            Time::SteadyTimePoint now) noexcept;
    }
}
