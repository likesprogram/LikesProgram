#include <LikesProgram/Quic/QuicStreamControlAction.hpp>

namespace LikesProgram {
    namespace Quic {
        Result<QuicActionMessage> BuildQuicResetStreamAction(
            const QuicResetStreamFrame& frame) {
            const auto encoded = BuildQuicResetStreamFrame(frame);
            if (!encoded.IsOk()) return encoded.GetStatus();
            QuicActionMessage action(
                QuicActionKind::ResetStream,
                frame.streamId,
                frame.applicationErrorCode);
            action.payload = Buffer(0);
            action.payload.Append(encoded.Value().data(), encoded.Value().size());
            return action;
        }

        Result<QuicActionMessage> BuildQuicStopSendingAction(
            const QuicStopSendingFrame& frame) {
            const auto encoded = BuildQuicStopSendingFrame(frame);
            if (!encoded.IsOk()) return encoded.GetStatus();
            QuicActionMessage action(
                QuicActionKind::StopSending,
                frame.streamId,
                frame.applicationErrorCode);
            action.payload = Buffer(0);
            action.payload.Append(encoded.Value().data(), encoded.Value().size());
            return action;
        }
    }
}
