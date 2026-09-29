#include <LikesProgram/Quic/QuicPathValidationAction.hpp>

#include <vector>

namespace LikesProgram {
    namespace Quic {
        Result<QuicActionMessage> BuildQuicPathResponseAction(
            const QuicPathValidationFrame& challenge) {
            const auto response = BuildQuicPathResponseFrame(challenge);
            if (!response.IsOk()) return response.GetStatus();
            const auto encoded = BuildQuicPathValidationFrame(response.Value());
            if (!encoded.IsOk()) return encoded.GetStatus();

            QuicActionMessage action(QuicActionKind::PathResponse);
            action.payload = Buffer(0);
            action.payload.Append(encoded.Value().data(), encoded.Value().size());
            return action;
        }
    }
}
