#include <LikesProgram/Quic/QuicEngine.hpp>
#include <LikesProgram/Quic/QuicHandshakeDoneFrame.hpp>

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
    using namespace LikesProgram::Quic;

    void Require(bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    }

    class HandshakeCompletionGate final {
    public:
        bool Accept(
            const QuicEngineOptions& options,
            const QuicHandshakeDoneFrame& frame,
            const QuicTlsHandshakeResult& result,
            QuicStreamEvent& event) noexcept {
            if (m_emitted || frame.consumedBytes != 1
                || result.state != QuicTlsHandshakeState::Complete
                || ValidateQuicTlsHandshakeResult(options, result)
                    != QuicTlsHandshakeError::None) {
                return false;
            }
            event = QuicStreamEvent{};
            event.kind = QuicEventKind::HandshakeComplete;
            m_emitted = true;
            return true;
        }

        void Reset() noexcept {
            m_emitted = false;
        }

        bool Emitted() const noexcept {
            return m_emitted;
        }

    private:
        bool m_emitted = false;
    };
}

int Run() {
    using namespace LikesProgram::Quic;
    const QuicEngineOptions options;
    const auto encoded = BuildQuicHandshakeDoneFrame();
    Require(encoded.IsOk() && encoded.Value().size() == 1
        && encoded.Value().front() == 0x1e,
        "HANDSHAKE_DONE should build as its one-byte frame type");

    auto withTrailing = encoded.Value();
    withTrailing.push_back(0xA5);
    const auto parsed = ParseQuicHandshakeDoneFrame(
        withTrailing.data(), withTrailing.size());
    Require(parsed.IsOk() && parsed.Value().consumedBytes == encoded.Value().size(),
        "HANDSHAKE_DONE should preserve its consumed boundary");

    QuicTlsHandshakeResult complete;
    complete.state = QuicTlsHandshakeState::Complete;
    complete.tlsVersion = QuicTlsVersion::Tls13;
    complete.applicationProtocol = "h3";
    complete.packetProtectionReady = true;
    Require(ValidateQuicTlsHandshakeResult(options, complete)
        == QuicTlsHandshakeError::None,
        "complete external TLS result should validate against engine options");

    QuicStreamEvent event;
    event.kind = QuicEventKind::StreamData;
    event.errorCode = 99;
    HandshakeCompletionGate gate;

    QuicTlsHandshakeResult progress;
    Require(!gate.Accept(options, parsed.Value(), progress, event)
        && event.kind == QuicEventKind::StreamData && event.errorCode == 99,
        "in-progress TLS result must not emit or mutate an event");

    auto failed = progress;
    failed.state = QuicTlsHandshakeState::Failed;
    failed.error = 77;
    Require(!gate.Accept(options, parsed.Value(), failed, event)
        && event.kind == QuicEventKind::StreamData && event.errorCode == 99,
        "failed TLS result must not emit or mutate an event");

    auto missingProtection = complete;
    missingProtection.packetProtectionReady = false;
    Require(!gate.Accept(options, parsed.Value(), missingProtection, event)
        && event.kind == QuicEventKind::StreamData && event.errorCode == 99,
        "missing packet protection must not emit an event");

    auto wrongProtocol = complete;
    wrongProtocol.applicationProtocol = std::string_view("h2");
    Require(!gate.Accept(options, parsed.Value(), wrongProtocol, event)
        && event.kind == QuicEventKind::StreamData && event.errorCode == 99,
        "ALPN mismatch must not emit an event");

    Require(gate.Accept(options, parsed.Value(), complete, event)
        && gate.Emitted()
        && event.kind == QuicEventKind::HandshakeComplete
        && event.streamId == 0 && event.errorCode == 0
        && !event.applicationError && event.payload.ReadableBytes() == 0,
        "valid HANDSHAKE_DONE plus complete TLS result should emit one event");
    Require(!gate.Accept(options, parsed.Value(), complete, event),
        "duplicate completion must not emit a second event");

    const std::uint8_t wrongFrame[] = { 0x1d };
    const std::uint8_t truncated[] = { 0 };
    Require(!ParseQuicHandshakeDoneFrame(wrongFrame, sizeof(wrongFrame)).IsOk()
        && !ParseQuicHandshakeDoneFrame(truncated, 0).IsOk(),
        "wrong and truncated HANDSHAKE_DONE input must be rejected");

    gate.Reset();
    Require(!gate.Emitted()
        && gate.Accept(options, parsed.Value(), complete, event)
        && event.kind == QuicEventKind::HandshakeComplete,
        "Reset must allow one fresh completion event");

    std::cout << "passed=true"
              << " frame_roundtrip=true"
              << " consumed_boundary=true"
              << " progress_suppressed=true"
              << " failure_suppressed=true"
              << " invalid_result_suppressed=true"
              << " completion_event=true"
              << " duplicate_suppressed=true"
              << " malformed_rejected=true"
              << " reset=true\n";
    return 0;
}

int main() {
    try {
        return Run();
    }
    catch (const std::exception& error) {
        std::cerr << "failed=" << error.what() << '\n';
        return 99;
    }
}
