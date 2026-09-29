#include <LikesProgram/Quic/QuicEngine.hpp>

#include <string_view>

namespace LikesProgram {
    namespace Quic {
        QuicTlsHandshakeError ValidateQuicTlsHandshakeResult(
            const QuicEngineOptions& options,
            const QuicTlsHandshakeResult& result) noexcept {
            if (!IsValidQuicEngineOptions(options)) {
                return QuicTlsHandshakeError::InvalidOptions;
            }
            if (result.state == QuicTlsHandshakeState::Failed) {
                return result.error == 0
                    ? QuicTlsHandshakeError::MissingFailureCode
                    : QuicTlsHandshakeError::None;
            }
            if (result.state == QuicTlsHandshakeState::InProgress) {
                return result.error == 0
                    ? QuicTlsHandshakeError::None
                    : QuicTlsHandshakeError::MissingFailureCode;
            }
            if (result.state != QuicTlsHandshakeState::Complete) {
                return QuicTlsHandshakeError::InvalidState;
            }
            if (result.tlsVersion != options.tlsVersion) {
                return QuicTlsHandshakeError::TlsVersionMismatch;
            }
            if (result.applicationProtocol.empty()
                || std::string_view(options.applicationProtocol)
                    != result.applicationProtocol) {
                return QuicTlsHandshakeError::ApplicationProtocolMismatch;
            }
            if (!result.packetProtectionReady) {
                return QuicTlsHandshakeError::PacketProtectionUnavailable;
            }
            return result.error == 0
                ? QuicTlsHandshakeError::None
                : QuicTlsHandshakeError::MissingFailureCode;
        }
        // 保持抽象 Engine 的销毁边界位于 Quic 二进制内。
        Result<void> QuicEngine::ApplyPeerFlowControl(
            const QuicFlowControlFrame&) {
            return Status(StatusCode::FailedPrecondition,
                u"QUIC engine does not expose peer flow-control synchronization");
        }

        QuicResult QuicEngine::BeginPathValidation(
            const Address&,
            const std::array<std::uint8_t, 8>&) {
            return { QuicAction::None,
                static_cast<int>(StatusCode::FailedPrecondition), {} };
        }

        QuicEngine::~QuicEngine() = default;
    }
}
