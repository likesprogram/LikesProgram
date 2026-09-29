#include <LikesProgram/Quic/QuicWireEngine.hpp>

#include <LikesProgram/Quic/QuicAckFrame.hpp>
#include <LikesProgram/Quic/QuicConnectionCloseFrame.hpp>
#include <LikesProgram/Quic/QuicConnectionIdFrame.hpp>
#include <LikesProgram/Quic/QuicCryptoFrame.hpp>
#include <LikesProgram/Quic/QuicFlowControlFrame.hpp>
#include <LikesProgram/Quic/QuicFrameType.hpp>
#include <LikesProgram/Quic/QuicHandshakeDoneFrame.hpp>
#include <LikesProgram/Quic/QuicLongHeader.hpp>
#include <LikesProgram/Quic/QuicPacketNumber.hpp>
#include <LikesProgram/Quic/QuicPathValidationFrame.hpp>
#include <LikesProgram/Quic/QuicPingFrame.hpp>
#include <LikesProgram/Quic/QuicShortHeader.hpp>
#include <LikesProgram/Quic/QuicStreamControlFrame.hpp>
#include <LikesProgram/Quic/QuicStreamFrame.hpp>
#include <LikesProgram/Quic/QuicVarInt.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <chrono>
#include <limits>
#include <span>
#include <utility>

namespace {
    using namespace LikesProgram::Quic;

    constexpr std::chrono::milliseconds kHandshakeTimer{ 25 };

    int ErrorCode(QuicWireEngineError error) noexcept {
        return static_cast<int>(error);
    }

    Buffer MakeBuffer(std::span<const std::uint8_t> bytes) {
        Buffer buffer(0);
        buffer.Append(bytes.data(), bytes.size());
        return buffer;
    }

    std::uint64_t DecodePacketNumber(
        std::span<const std::uint8_t> bytes) noexcept {
        std::uint64_t value = 0;
        for (const auto byte : bytes) value = (value << 8) | byte;
        return value;
    }

    QuicPacketProtectionLevel ProtectionLevel(QuicLongPacketType type) noexcept {
        switch (type) {
        case QuicLongPacketType::Initial: return QuicPacketProtectionLevel::Initial;
        case QuicLongPacketType::Handshake: return QuicPacketProtectionLevel::Handshake;
        case QuicLongPacketType::ZeroRtt: return QuicPacketProtectionLevel::ZeroRtt;
        default: return QuicPacketProtectionLevel::Initial;
        }
    }

    bool SamePeer(const LikesProgram::Net::Address& left,
        const LikesProgram::Net::Address& right) noexcept {
        return left.IsValid() && right.IsValid()
            && left.FamilyValue() == right.FamilyValue()
            && left.Port() == right.Port()
            && left.Ip() == right.Ip();
    }

    bool IsCandidatePathPacket(
        std::span<const std::uint8_t> plaintext,
        const std::array<std::uint8_t, 8>& expectedResponse) {
        std::size_t offset = 0;
        bool sawResponse = false;
        while (offset < plaintext.size()) {
            const auto remaining = plaintext.subspan(offset);
            const auto type = ParseQuicFrameType(
                remaining.data(), remaining.size());
            if (!type.IsOk()) return false;

            std::size_t consumed = 0;
            switch (type.Value().kind) {
            case QuicFrameType::Padding:
                consumed = type.Value().encodedBytes;
                break;
            case QuicFrameType::Ping: {
                const auto frame = ParseQuicPingFrame(
                    remaining.data(), remaining.size());
                if (!frame.IsOk()) return false;
                consumed = frame.Value().consumedBytes;
                break;
            }
            case QuicFrameType::Ack: {
                const auto frame = ParseQuicAckFrame(
                    remaining.data(), remaining.size());
                if (!frame.IsOk()) return false;
                consumed = frame.Value().consumedBytes;
                break;
            }
            case QuicFrameType::PathChallenge:
            case QuicFrameType::PathResponse: {
                const auto frame = ParseQuicPathValidationFrame(
                    remaining.data(), remaining.size());
                if (!frame.IsOk()) return false;
                if (frame.Value().kind
                    == QuicPathValidationFrameKind::PathResponse) {
                    if (sawResponse || frame.Value().data != expectedResponse) {
                        return false;
                    }
                    sawResponse = true;
                }
                consumed = frame.Value().consumedBytes;
                break;
            }
            default:
                return false;
            }
            if (consumed == 0 || consumed > remaining.size()) return false;
            offset += consumed;
        }
        return offset == plaintext.size();
    }
}

namespace LikesProgram {
    namespace Quic {
        QuicWireEngine::QuicWireEngine(const QuicEngineOptions& options)
            : m_options(options),
              m_applicationProtocol(options.applicationProtocol == nullptr
                  ? "" : options.applicationProtocol),
              m_packetHeaderProtectionProvider(options.packetHeaderProtectionProvider) {
            m_options.applicationProtocol = m_applicationProtocol.c_str();
            if (!IsValidQuicEngineOptions(options)) {
                m_state = QuicState::Failed;
                m_lastError = ErrorCode(QuicWireEngineError::InvalidOptions);
                return;
            }
            const auto congestionLimit =
                m_creditCongestionLedger.Limits().congestion.maxWindowBytes;
            if (!m_creditCongestionLedger.SetCongestionWindow(
                    congestionLimit).IsOk()) {
                m_state = QuicState::Failed;
                m_lastError = ErrorCode(QuicWireEngineError::InvalidOptions);
            }
        }

        QuicWireEngine::~QuicWireEngine() = default;

        QuicResult QuicWireEngine::QueueTlsHandshakeData(Buffer&& data) {
            if (m_state != QuicState::Handshaking || m_started
                || !m_protectedDatagram.empty()) {
                return Fail(QuicWireEngineError::InvalidState);
            }
            const auto readable = data.ReadableBytes();
            m_tlsHandshakeData.clear();
            if (readable > 0) {
                m_tlsHandshakeData.assign(data.Peek(), data.Peek() + readable);
            }
            data.RetrieveAll();
            return {};
        }

        QuicResult QuicWireEngine::QueueProtectedDatagram(Buffer&& datagram) {
            const auto readable = datagram.ReadableBytes();
            if (readable == 0) {
                return Fail(QuicWireEngineError::InvalidState);
            }
            if (readable > m_options.maximumDatagramBytes) {
                return Fail(QuicWireEngineError::DatagramTooLarge);
            }
            if (m_state == QuicState::Handshaking && !m_started) {
                m_protectedDatagram.assign(
                    datagram.Peek(), datagram.Peek() + readable);
                datagram.RetrieveAll();
                return {};
            }
            // The caller owns TLS/packet protection; the Engine only delivers
            // the complete datagram and preserves the current peer address.
            if (m_started && (m_state == QuicState::Handshaking
                    || m_state == QuicState::Active)) {
                QuicActionMessage action(QuicActionKind::DatagramReady);
                action.peer = m_peer;
                action.payload = MakeBuffer({ datagram.Peek(), readable });
                datagram.RetrieveAll();
                if (!EmitAction(std::move(action))) {
                    return Fail(QuicWireEngineError::ActionRejected);
                }
                return { QuicAction::DatagramReady, 0, {} };
            }
            return Fail(QuicWireEngineError::InvalidState);
        }

        Result<void> QuicWireEngine::ApplyPeerFlowControl(
            const QuicFlowControlFrame& frame) {
            if (!m_started
                || (m_state != QuicState::Handshaking
                    && m_state != QuicState::Active)) {
                return Status(StatusCode::FailedPrecondition,
                    u"QUIC peer flow control requires a started connection");
            }
            return m_creditCongestionLedger.ApplyPeerFlowControl(frame);
        }

        QuicResult QuicWireEngine::BeginPathValidation(
            const Address& candidate,
            const std::array<std::uint8_t, 8>& token) {
            if (m_state != QuicState::Active || !m_peer.IsValid()
                || !candidate.IsValid() || SamePeer(m_peer, candidate)
                || m_hasCandidatePeer) {
                return Reject(QuicWireEngineError::InvalidState);
            }
            const auto frame = BuildQuicPathValidationFrame({
                QuicPathValidationFrameKind::PathChallenge, token, 0 });
            if (!frame.IsOk()) return Reject(QuicWireEngineError::InvalidDatagram);
            const auto begun = m_pathMigrationLedger.BeginValidation(token);
            if (!begun.IsOk()) return Reject(QuicWireEngineError::InvalidState);

            const auto previousState = m_pathState;
            m_candidatePeer = candidate;
            m_hasCandidatePeer = true;
            m_pathState = QuicPathState::Validating;
            const auto sent = SendProtectedFrameTo(frame.Value(), candidate);
            if (!sent.Succeeded()) {
                m_pathMigrationLedger.CancelValidation();
                m_candidatePeer = Address();
                m_hasCandidatePeer = false;
                m_pathState = previousState;
            }
            return sent;
        }

        void QuicWireEngine::SetActionSink(QuicActionSink* sink) noexcept {
            m_actionSink = sink;
        }

        void QuicWireEngine::SetEventObserver(QuicEventObserver* observer) noexcept {
            m_eventObserver = observer;
        }

        void QuicWireEngine::SetPacketProtectionProvider(
            QuicPacketProtectionProvider* provider) noexcept {
            m_packetProtectionProvider = provider;
        }

        QuicPacketProtectionResult QuicWireEngine::ProtectPacket(
            const QuicPacketProtectionRequest& request,
            std::span<std::uint8_t> output) noexcept {
            const auto validation = ValidateQuicPacketProtectionRequest(
                request, std::span<const std::uint8_t>(output.data(), output.size()));
            if (validation != QuicPacketProtectionError::None) {
                return { validation, 0 };
            }
            if (m_packetProtectionProvider == nullptr) {
                return { QuicPacketProtectionError::NotReady, 0 };
            }
            return m_packetProtectionProvider->Protect(request, output);
        }

        QuicPacketProtectionResult QuicWireEngine::UnprotectPacket(
            const QuicPacketProtectionRequest& request,
            std::span<std::uint8_t> output) noexcept {
            const auto validation = ValidateQuicPacketProtectionRequest(
                request, std::span<const std::uint8_t>(output.data(), output.size()));
            if (validation != QuicPacketProtectionError::None) {
                return { validation, 0 };
            }
            if (m_packetProtectionProvider == nullptr) {
                return { QuicPacketProtectionError::NotReady, 0 };
            }
            return m_packetProtectionProvider->Unprotect(request, output);
        }

        QuicResult QuicWireEngine::StartHandshake() {
            if (m_state != QuicState::Handshaking || m_started) {
                return Fail(QuicWireEngineError::InvalidState);
            }
            if (m_actionSink == nullptr) {
                return Fail(QuicWireEngineError::MissingActionSink);
            }

            m_started = true;
            QuicAction actions = QuicAction::None;
            if (m_options.role == QuicRole::Client) {
                const auto initial = BuildAndEmitInitial();
                if (!initial.Succeeded()) return initial;
                actions = initial.actions;
            }

            QuicActionMessage timer(QuicActionKind::ArmTimer);
            timer.timerDelay = kHandshakeTimer;
            if (!EmitAction(std::move(timer))) {
                return Fail(QuicWireEngineError::ActionRejected);
            }
            actions = actions | QuicAction::ArmTimer;
            return { actions, 0, kHandshakeTimer };
        }

        QuicResult QuicWireEngine::ProvideTlsHandshakeResult(
            const QuicTlsHandshakeResult& result) {
            if (!m_started || m_state != QuicState::Handshaking) {
                return Fail(QuicWireEngineError::InvalidState);
            }
            const auto validation = ValidateQuicTlsHandshakeResult(m_options, result);
            if (validation != QuicTlsHandshakeError::None) {
                return Fail(static_cast<QuicWireEngineError>(
                    static_cast<int>(validation) + 2100));
            }
            if (result.state == QuicTlsHandshakeState::Failed) {
                m_state = QuicState::Failed;
                m_lastError = result.error;
                return { QuicAction::None, result.error, {} };
            }
            if (result.state == QuicTlsHandshakeState::InProgress) return {};
            if (m_packetProtectionProvider == nullptr) {
                return Fail(QuicWireEngineError::PacketProtectionUnavailable);
            }

            m_state = QuicState::Active;
            m_tlsVersion = result.tlsVersion;
            m_negotiatedProtocol.assign(result.applicationProtocol);
            m_packetProtectionReady = result.packetProtectionReady;
            EmitEvent(QuicEventKind::HandshakeComplete);

            QuicActionMessage cancel(QuicActionKind::CancelTimer);
            if (!EmitAction(std::move(cancel))) {
                return Fail(QuicWireEngineError::ActionRejected);
            }
            return { QuicAction::StreamEvent | QuicAction::CancelTimer, 0, {} };
        }

        QuicResult QuicWireEngine::ReassembleStreamFrame(
            const QuicStreamFrame& frame) {
            if (frame.streamId > kQuicVarIntMaximum) {
                return Fail(QuicWireEngineError::InvalidDatagram);
            }
            const auto offset = frame.hasOffset ? frame.offset : 0;
            const auto dataSize = frame.data.size();
            if (offset > kQuicVarIntMaximum
                || dataSize > kQuicVarIntMaximum - offset) {
                return Fail(QuicWireEngineError::InvalidDatagram);
            }

            auto stateIt = m_streamReceiveStates.find(frame.streamId);
            if (stateIt == m_streamReceiveStates.end()) {
                stateIt = m_streamReceiveStates.emplace(
                    frame.streamId, StreamReceiveState{}).first;
            }
            auto& state = stateIt->second;
            if (state.finished || offset < state.nextOffset
                || state.pending.find(offset) != state.pending.end()
                || dataSize > m_options.maximumDatagramBytes - state.pendingBytes) {
                return Fail(QuicWireEngineError::InvalidDatagram);
            }

            const auto end = offset + dataSize;
            for (const auto& [pendingOffset, pending] : state.pending) {
                const auto pendingSize = pending.data.size();
                const auto pendingEnd = pendingOffset + pendingSize;
                if (pendingSize != 0 && dataSize != 0
                    && offset < pendingEnd && pendingOffset < end) {
                    return Fail(QuicWireEngineError::InvalidDatagram);
                }
                if ((pending.fin && offset >= pendingEnd)
                    || (frame.fin && pendingOffset >= end)) {
                    return Fail(QuicWireEngineError::InvalidDatagram);
                }
            }

            PendingStreamFrame pending;
            pending.fin = frame.fin;
            pending.data.assign(frame.data.begin(), frame.data.end());
            state.pending.emplace(offset, std::move(pending));
            state.pendingBytes += dataSize;

            std::vector<std::uint8_t> contiguous;
            contiguous.reserve(state.pendingBytes);
            bool hasContiguousFrame = false;
            bool fin = false;
            while (true) {
                const auto next = state.pending.find(state.nextOffset);
                if (next == state.pending.end()) break;
                hasContiguousFrame = true;
                contiguous.insert(contiguous.end(), next->second.data.begin(),
                    next->second.data.end());
                state.nextOffset += next->second.data.size();
                state.pendingBytes -= next->second.data.size();
                fin = next->second.fin;
                state.pending.erase(next);
                if (fin) {
                    state.finished = true;
                    break;
                }
            }

            if (hasContiguousFrame) {
                Buffer payload(0);
                payload.Append(contiguous.data(), contiguous.size());
                EmitEvent(
                    fin ? QuicEventKind::StreamFin : QuicEventKind::StreamData,
                    frame.streamId,
                    std::move(payload));
                return { QuicAction::StreamEvent, 0, {} };
            }
            return {};
        }

        QuicResult QuicWireEngine::DispatchOneRttFrames(
            const Address& peer,
            std::span<const std::uint8_t> plaintext) {
            if (plaintext.empty()) return Fail(QuicWireEngineError::InvalidDatagram);

            if (!m_peer.IsValid()) m_peer = peer;
            const bool currentPeer = SamePeer(m_peer, peer);
            const bool candidatePeer = m_hasCandidatePeer
                && SamePeer(m_candidatePeer, peer);
            const auto pathSnapshot = m_pathMigrationLedger.Snapshot();
            if (!currentPeer
                && (!candidatePeer
                    || !IsCandidatePathPacket(
                        plaintext, pathSnapshot.probe.token))) {
                return Reject(QuicWireEngineError::InvalidDatagram);
            }
            QuicAction actions = QuicAction::None;
            std::size_t offset = 0;
            while (offset < plaintext.size()) {
                const auto remaining = plaintext.subspan(offset);
                const auto type = ParseQuicFrameType(
                    remaining.data(), remaining.size());
                if (!type.IsOk() || type.Value().kind == QuicFrameType::Unknown) {
                    return Fail(QuicWireEngineError::UnsupportedPacket);
                }

                std::size_t consumed = 0;
                switch (type.Value().kind) {
                case QuicFrameType::Padding:
                    consumed = type.Value().encodedBytes;
                    break;
                case QuicFrameType::Ping: {
                    const auto frame = ParseQuicPingFrame(
                        remaining.data(), remaining.size());
                    if (!frame.IsOk()) return Fail(QuicWireEngineError::InvalidDatagram);
                    consumed = frame.Value().consumedBytes;
                    break;
                }
                case QuicFrameType::Ack: {
                    const auto frame = ParseQuicAckFrame(
                        remaining.data(), remaining.size());
                    if (!frame.IsOk()
                        || !m_ackRecoveryLedger.ApplyAck(frame.Value()).IsOk()) {
                        return Fail(QuicWireEngineError::InvalidDatagram);
                    }
                    ForgetAcknowledgedPacketPeers(frame.Value());
                    const auto creditAcknowledged =
                        m_creditCongestionLedger.Acknowledge(
                            frame.Value().largestAcknowledged);
                    if (!creditAcknowledged.IsOk()
                        && creditAcknowledged.GetStatus().Code()
                            != StatusCode::NotFound) {
                        return Fail(QuicWireEngineError::InvalidDatagram);
                    }
                    consumed = frame.Value().consumedBytes;
                    break;
                }
                case QuicFrameType::ResetStream: {
                    const auto frame = ParseQuicResetStreamFrame(
                        remaining.data(), remaining.size());
                    if (!frame.IsOk()) return Fail(QuicWireEngineError::InvalidDatagram);
                    EmitEvent(
                        QuicEventKind::StreamReset,
                        frame.Value().streamId,
                        Buffer(0),
                        frame.Value().applicationErrorCode);
                    actions = actions | QuicAction::StreamEvent;
                    consumed = frame.Value().consumedBytes;
                    break;
                }
                case QuicFrameType::StopSending: {
                    const auto frame = ParseQuicStopSendingFrame(
                        remaining.data(), remaining.size());
                    if (!frame.IsOk()) return Fail(QuicWireEngineError::InvalidDatagram);
                    EmitEvent(
                        QuicEventKind::StopSending,
                        frame.Value().streamId,
                        Buffer(0),
                        frame.Value().applicationErrorCode);
                    actions = actions | QuicAction::StreamEvent;
                    consumed = frame.Value().consumedBytes;
                    break;
                }
                case QuicFrameType::Crypto: {
                    const auto frame = ParseQuicCryptoFrame(
                        remaining.data(), remaining.size());
                    if (!frame.IsOk()) return Fail(QuicWireEngineError::InvalidDatagram);
                    EmitEvent(
                        QuicEventKind::CryptoData,
                        frame.Value().offset,
                        MakeBuffer(frame.Value().data));
                    actions = actions | QuicAction::StreamEvent;
                    consumed = frame.Value().consumedBytes;
                    break;
                }
                case QuicFrameType::Stream: {
                    const auto frame = ParseQuicStreamFrame(
                        remaining.data(), remaining.size());
                    if (!frame.IsOk()) return Fail(QuicWireEngineError::InvalidDatagram);
                    const auto result = ReassembleStreamFrame(frame.Value());
                    if (!result.Succeeded()) return result;
                    actions = actions | result.actions;
                    consumed = frame.Value().consumedBytes;
                    break;
                }
                case QuicFrameType::MaxData:
                case QuicFrameType::MaxStreamData: {
                    const auto frame = ParseQuicFlowControlFrame(
                        remaining.data(), remaining.size());
                    if (!frame.IsOk()
                        || !m_creditCongestionLedger.ApplyPeerFlowControl(
                            frame.Value()).IsOk()) {
                        return Fail(QuicWireEngineError::InvalidDatagram);
                    }
                    consumed = frame.Value().consumedBytes;
                    break;
                }
                case QuicFrameType::NewConnectionId:
                case QuicFrameType::RetireConnectionId: {
                    const auto frame = ParseQuicConnectionIdFrame(
                        remaining.data(), remaining.size());
                    if (!frame.IsOk()
                        || !m_pathMigrationLedger.ObserveConnectionId(
                            frame.Value()).IsOk()) {
                        return Fail(QuicWireEngineError::InvalidDatagram);
                    }
                    consumed = frame.Value().consumedBytes;
                    break;
                }
                case QuicFrameType::PathChallenge:
                case QuicFrameType::PathResponse: {
                    const auto frame = ParseQuicPathValidationFrame(
                        remaining.data(), remaining.size());
                    if (!frame.IsOk()) return Fail(QuicWireEngineError::InvalidDatagram);
                    consumed = frame.Value().consumedBytes;
                    if (frame.Value().kind
                        == QuicPathValidationFrameKind::PathChallenge) {
                        const auto response = BuildQuicPathValidationFrame({
                            QuicPathValidationFrameKind::PathResponse,
                            frame.Value().data,
                            0
                        });
                        if (!response.IsOk()) {
                            return Fail(QuicWireEngineError::InvalidDatagram);
                        }
                        const auto send = SendProtectedFrameTo(
                            response.Value(), peer);
                        if (!send.Succeeded()) return send;
                        actions = actions | send.actions;
                    }
                    else {
                        if (!candidatePeer) {
                            return Reject(QuicWireEngineError::InvalidDatagram);
                        }
                        const auto response =
                            m_pathMigrationLedger.ObserveResponse(frame.Value());
                        if (!response.IsOk()) {
                            return Reject(QuicWireEngineError::InvalidDatagram);
                        }
                        m_peer = m_candidatePeer;
                        m_candidatePeer = Address();
                        m_hasCandidatePeer = false;
                        m_pathState = QuicPathState::Migrated;
                        EmitEvent(QuicEventKind::PathMigrated);
                        actions = actions | QuicAction::PathEvent;
                    }
                    break;
                }
                case QuicFrameType::ConnectionClose: {
                    const auto frame = ParseQuicConnectionCloseFrame(
                        remaining.data(), remaining.size());
                    if (!frame.IsOk()) return Fail(QuicWireEngineError::InvalidDatagram);
                    Buffer reason(0);
                    if (!frame.Value().reason.empty()) {
                        reason.Append(
                            frame.Value().reason.data(), frame.Value().reason.size());
                    }
                    EmitEvent(
                        QuicEventKind::ConnectionClose,
                        0,
                        std::move(reason),
                        frame.Value().errorCode,
                        frame.Value().application);
                    m_state = QuicState::Closed;
                    actions = actions | QuicAction::ConnectionClose;
                    consumed = frame.Value().consumedBytes;
                    const auto trailing = remaining.subspan(consumed);
                    if (!std::all_of(
                            trailing.begin(), trailing.end(),
                            [](std::uint8_t byte) { return byte == 0; })) {
                        return Fail(QuicWireEngineError::InvalidDatagram);
                    }
                    offset = plaintext.size();
                    continue;
                }
                case QuicFrameType::HandshakeDone: {
                    const auto frame = ParseQuicHandshakeDoneFrame(
                        remaining.data(), remaining.size());
                    if (!frame.IsOk()) return Fail(QuicWireEngineError::InvalidDatagram);
                    consumed = frame.Value().consumedBytes;
                    break;
                }
                default:
                    return Fail(QuicWireEngineError::UnsupportedPacket);
                }

                if (consumed == 0 || consumed > remaining.size()) {
                    return Fail(QuicWireEngineError::InvalidDatagram);
                }
                offset += consumed;
            }
            return { actions, 0, {} };
        }

        QuicResult QuicWireEngine::DispatchLongHeaderFrames(
            QuicLongPacketType packetType,
            std::span<const std::uint8_t> plaintext) {
            if (plaintext.empty()) return Fail(QuicWireEngineError::InvalidDatagram);

            QuicAction actions = QuicAction::None;
            std::size_t offset = 0;
            while (offset < plaintext.size()) {
                const auto remaining = plaintext.subspan(offset);
                const auto type = ParseQuicFrameType(
                    remaining.data(), remaining.size());
                if (!type.IsOk() || type.Value().kind == QuicFrameType::Unknown) {
                    return Fail(QuicWireEngineError::UnsupportedPacket);
                }

                std::size_t consumed = 0;
                switch (type.Value().kind) {
                case QuicFrameType::Padding:
                    consumed = type.Value().encodedBytes;
                    break;
                case QuicFrameType::Ping: {
                    const auto frame = ParseQuicPingFrame(
                        remaining.data(), remaining.size());
                    if (!frame.IsOk()) return Fail(QuicWireEngineError::InvalidDatagram);
                    consumed = frame.Value().consumedBytes;
                    break;
                }
                case QuicFrameType::Ack: {
                    if (packetType == QuicLongPacketType::ZeroRtt) {
                        return Fail(QuicWireEngineError::UnsupportedPacket);
                    }
                    const auto frame = ParseQuicAckFrame(
                        remaining.data(), remaining.size());
                    if (!frame.IsOk()
                        || !m_ackRecoveryLedger.ApplyAck(frame.Value()).IsOk()) {
                        return Fail(QuicWireEngineError::InvalidDatagram);
                    }
                    ForgetAcknowledgedPacketPeers(frame.Value());
                    const auto creditAcknowledged =
                        m_creditCongestionLedger.Acknowledge(
                            frame.Value().largestAcknowledged);
                    if (!creditAcknowledged.IsOk()
                        && creditAcknowledged.GetStatus().Code()
                            != StatusCode::NotFound) {
                        return Fail(QuicWireEngineError::InvalidDatagram);
                    }
                    consumed = frame.Value().consumedBytes;
                    break;
                }
                case QuicFrameType::Crypto: {
                    if (packetType == QuicLongPacketType::ZeroRtt) {
                        return Fail(QuicWireEngineError::UnsupportedPacket);
                    }
                    const auto frame = ParseQuicCryptoFrame(
                        remaining.data(), remaining.size());
                    if (!frame.IsOk()) return Fail(QuicWireEngineError::InvalidDatagram);
                    EmitEvent(
                        QuicEventKind::CryptoData,
                        frame.Value().offset,
                        MakeBuffer(frame.Value().data));
                    actions = actions | QuicAction::StreamEvent;
                    consumed = frame.Value().consumedBytes;
                    break;
                }
                case QuicFrameType::ResetStream: {
                    if (packetType != QuicLongPacketType::ZeroRtt) {
                        return Fail(QuicWireEngineError::UnsupportedPacket);
                    }
                    const auto frame = ParseQuicResetStreamFrame(
                        remaining.data(), remaining.size());
                    if (!frame.IsOk()) return Fail(QuicWireEngineError::InvalidDatagram);
                    EmitEvent(
                        QuicEventKind::StreamReset,
                        frame.Value().streamId,
                        Buffer(0),
                        frame.Value().applicationErrorCode);
                    actions = actions | QuicAction::StreamEvent;
                    consumed = frame.Value().consumedBytes;
                    break;
                }
                case QuicFrameType::StopSending: {
                    if (packetType != QuicLongPacketType::ZeroRtt) {
                        return Fail(QuicWireEngineError::UnsupportedPacket);
                    }
                    const auto frame = ParseQuicStopSendingFrame(
                        remaining.data(), remaining.size());
                    if (!frame.IsOk()) return Fail(QuicWireEngineError::InvalidDatagram);
                    EmitEvent(
                        QuicEventKind::StopSending,
                        frame.Value().streamId,
                        Buffer(0),
                        frame.Value().applicationErrorCode);
                    actions = actions | QuicAction::StreamEvent;
                    consumed = frame.Value().consumedBytes;
                    break;
                }
                case QuicFrameType::Stream: {
                    if (packetType != QuicLongPacketType::ZeroRtt) {
                        return Fail(QuicWireEngineError::UnsupportedPacket);
                    }
                    const auto frame = ParseQuicStreamFrame(
                        remaining.data(), remaining.size());
                    if (!frame.IsOk()) return Fail(QuicWireEngineError::InvalidDatagram);
                    const auto result = ReassembleStreamFrame(frame.Value());
                    if (!result.Succeeded()) return result;
                    actions = actions | result.actions;
                    consumed = frame.Value().consumedBytes;
                    break;
                }
                case QuicFrameType::MaxData:
                case QuicFrameType::MaxStreamData: {
                    if (packetType != QuicLongPacketType::ZeroRtt) {
                        return Fail(QuicWireEngineError::UnsupportedPacket);
                    }
                    const auto frame = ParseQuicFlowControlFrame(
                        remaining.data(), remaining.size());
                    if (!frame.IsOk()
                        || !m_creditCongestionLedger.ApplyPeerFlowControl(
                            frame.Value()).IsOk()) {
                        return Fail(QuicWireEngineError::InvalidDatagram);
                    }
                    consumed = frame.Value().consumedBytes;
                    break;
                }
                case QuicFrameType::ConnectionClose: {
                    const auto frame = ParseQuicConnectionCloseFrame(
                        remaining.data(), remaining.size());
                    if (!frame.IsOk()) return Fail(QuicWireEngineError::InvalidDatagram);
                    Buffer reason(0);
                    if (!frame.Value().reason.empty()) {
                        reason.Append(
                            frame.Value().reason.data(), frame.Value().reason.size());
                    }
                    EmitEvent(
                        QuicEventKind::ConnectionClose,
                        0,
                        std::move(reason),
                        frame.Value().errorCode,
                        frame.Value().application);
                    m_state = QuicState::Closed;
                    actions = actions | QuicAction::ConnectionClose;
                    consumed = frame.Value().consumedBytes;
                    if (!std::all_of(
                            remaining.subspan(consumed).begin(),
                            remaining.subspan(consumed).end(),
                            [](std::uint8_t byte) { return byte == 0; })) {
                        return Fail(QuicWireEngineError::InvalidDatagram);
                    }
                    offset = plaintext.size();
                    continue;
                }
                default:
                    return Fail(QuicWireEngineError::UnsupportedPacket);
                }

                if (consumed == 0 || consumed > remaining.size()) {
                    return Fail(QuicWireEngineError::InvalidDatagram);
                }
                offset += consumed;
            }
            return { actions, 0, {} };
        }

        QuicResult QuicWireEngine::ConsumeDatagram(
            const Address& peer,
            Buffer&& datagram) {
            if (!m_started || m_state == QuicState::Closed
                || m_state == QuicState::Failed) {
                return Fail(QuicWireEngineError::InvalidState);
            }
            const auto size = datagram.ReadableBytes();
            if (size == 0 || size > m_options.maximumDatagramBytes) {
                return Fail(size == 0
                    ? QuicWireEngineError::InvalidDatagram
                    : QuicWireEngineError::DatagramTooLarge);
            }
            const auto* begin = datagram.Peek();

            if ((begin[0] & 0x80) == 0) {
                if (m_state != QuicState::Active) {
                    return Fail(QuicWireEngineError::UnsupportedPacket);
                }
                if (m_packetHeaderProtectionProvider == nullptr
                    || m_packetProtectionProvider == nullptr) {
                    return Fail(QuicWireEngineError::PacketProtectionUnavailable);
                }

                std::vector<std::uint8_t> unprotected(m_options.maximumDatagramBytes);
                const auto headerProtection =
                    m_packetHeaderProtectionProvider->UnprotectShortHeader(
                        {
                            QuicPacketProtectionLevel::OneRtt,
                            m_hasReceivedPacketNumber ? m_largestReceivedPacketNumber : 0,
                            m_options.destinationConnectionIdLength,
                            { begin, size }
                        },
                        unprotected);
                const QuicShortHeaderProtectionRequest headerRequest{
                    QuicPacketProtectionLevel::OneRtt,
                    m_hasReceivedPacketNumber ? m_largestReceivedPacketNumber : 0,
                    m_options.destinationConnectionIdLength,
                    { begin, size }
                };
                if (ValidateQuicShortHeaderProtectionResult(
                        headerRequest,
                        headerProtection,
                        std::span<const std::uint8_t>(
                            unprotected.data(), unprotected.size()))
                    != QuicPacketHeaderProtectionError::None) {
                    return Fail(QuicWireEngineError::InvalidDatagram);
                }

                const auto shortHeader = ParseQuicShortHeader(
                    unprotected.data(), headerProtection.bytesWritten,
                    m_options.destinationConnectionIdLength);
                if (!shortHeader.IsOk()
                    || shortHeader.Value().consumedBytes != headerProtection.bytesWritten) {
                    return Fail(QuicWireEngineError::InvalidDatagram);
                }

                std::vector<std::uint8_t> plaintext(m_options.maximumDatagramBytes);
                const auto* payloadBegin = shortHeader.Value().payload.data();
                const auto associatedDataSize = static_cast<std::size_t>(
                    payloadBegin - unprotected.data());
                const auto protection = UnprotectPacket({
                    QuicPacketProtectionLevel::OneRtt,
                    headerProtection.packetNumber,
                    { unprotected.data(), associatedDataSize },
                    shortHeader.Value().payload
                }, plaintext);
                if (!protection.Succeeded() || protection.bytesWritten == 0
                    || protection.bytesWritten > plaintext.size()) {
                    return Fail(QuicWireEngineError::PacketProtectionUnavailable);
                }

                const auto dispatched = DispatchOneRttFrames(
                    peer,
                    { plaintext.data(), protection.bytesWritten });
                if (!dispatched.Succeeded()) return dispatched;
                ++m_receivedDatagrams;
                if (m_pathState == QuicPathState::Unknown) {
                    m_pathState = QuicPathState::Validating;
                }
                if (!m_hasReceivedPacketNumber
                    || headerProtection.packetNumber > m_largestReceivedPacketNumber) {
                    m_largestReceivedPacketNumber = headerProtection.packetNumber;
                    m_hasReceivedPacketNumber = true;
                }
                return dispatched;
            }

            auto parsed = ParseQuicLongHeader(begin, size);
            if (!parsed.IsOk() || parsed.Value().consumedBytes != size) {
                return Fail(QuicWireEngineError::InvalidDatagram);
            }
            auto header = parsed.Value();
            if (header.type == QuicLongPacketType::Retry
                || header.type == QuicLongPacketType::VersionNegotiation) {
                return Fail(QuicWireEngineError::UnsupportedPacket);
            }
            if (m_state == QuicState::Active
                && header.type != QuicLongPacketType::Initial
                && header.type != QuicLongPacketType::Handshake) {
                return Fail(QuicWireEngineError::UnsupportedPacket);
            }

            // Long-header packet number length and the low header bits are
            // protected on the wire. Providers remain caller-owned; a
            // short-header-only provider reports NotReady and keeps the
            // legacy unprotected-header path working for contract tests.
            std::vector<std::uint8_t> unprotectedHeader;
            const std::uint8_t* protectedPacketBegin = begin;
            std::uint64_t protectedPacketNumber = 0;
            bool longHeaderWasUnprotected = false;
            if (m_packetHeaderProtectionProvider != nullptr
                && size >= header.packetNumberOffset + 4 + 16) {
                unprotectedHeader.resize(m_options.maximumDatagramBytes);
                const QuicLongHeaderProtectionRequest headerRequest{
                    ProtectionLevel(header.type),
                    [&] {
                        QuicPacketNumberSpace* space = &m_initialPacketNumbers;
                        if (header.type == QuicLongPacketType::Handshake) {
                            space = &m_handshakePacketNumbers;
                        }
                        else if (header.type == QuicLongPacketType::ZeroRtt) {
                            space = &m_zeroRttPacketNumbers;
                        }
                        const auto snapshot = space->Snapshot();
                        return snapshot.hasLargestReceived
                            ? snapshot.largestReceived : 0;
                    }(),
                    header.packetNumberOffset,
                    0,
                    { begin, size }
                };
                const auto headerProtection =
                    m_packetHeaderProtectionProvider->UnprotectLongHeader(
                        headerRequest, unprotectedHeader);
                if (headerProtection.error
                    != QuicPacketHeaderProtectionError::NotReady) {
                    if (ValidateQuicLongHeaderProtectionResult(
                            headerRequest,
                            headerProtection,
                            std::span<const std::uint8_t>(
                                unprotectedHeader.data(), unprotectedHeader.size()))
                        != QuicPacketHeaderProtectionError::None) {
                        return Fail(QuicWireEngineError::InvalidDatagram);
                    }
                    auto unprotectedParsed = ParseQuicLongHeader(
                        unprotectedHeader.data(), headerProtection.bytesWritten);
                    if (!unprotectedParsed.IsOk()
                        || unprotectedParsed.Value().consumedBytes
                            != headerProtection.bytesWritten
                        || unprotectedParsed.Value().packetNumberLength
                            != headerProtection.packetNumberLength
                        || unprotectedParsed.Value().packetNumberOffset
                            != headerRequest.packetNumberOffset) {
                        return Fail(QuicWireEngineError::InvalidDatagram);
                    }
                    header = unprotectedParsed.Value();
                    protectedPacketBegin = unprotectedHeader.data();
                    protectedPacketNumber = headerProtection.packetNumber;
                    longHeaderWasUnprotected = true;
                }
            }

            QuicPacketNumberSpace* packetNumbers = &m_initialPacketNumbers;
            if (header.type == QuicLongPacketType::Handshake) {
                packetNumbers = &m_handshakePacketNumbers;
            }
            else if (header.type == QuicLongPacketType::ZeroRtt) {
                packetNumbers = &m_zeroRttPacketNumbers;
            }
            const auto numberSnapshot = packetNumbers->Snapshot();
            std::uint64_t fullPacketNumber = 0;
            if (longHeaderWasUnprotected) {
                fullPacketNumber = protectedPacketNumber;
            }
            else {
                const auto decodedPacketNumber = DecodeQuicPacketNumber(
                    numberSnapshot.hasLargestReceived
                        ? numberSnapshot.largestReceived : 0,
                    DecodePacketNumber(header.packetNumber),
                    header.packetNumber.size());
                if (!decodedPacketNumber.IsOk()) {
                    return Fail(QuicWireEngineError::InvalidDatagram);
                }
                fullPacketNumber = decodedPacketNumber.Value();
            }

            if (m_peer.IsValid() && !SamePeer(m_peer, peer)) {
                return Reject(QuicWireEngineError::InvalidDatagram);
            }
            ++m_receivedDatagrams;
            m_peer = peer;
            if (m_pathState == QuicPathState::Unknown) {
                m_pathState = QuicPathState::Validating;
            }

            std::vector<std::uint8_t> plaintext;
            std::span<const std::uint8_t> cryptoPayload = header.payload;
            auto crypto = ParseQuicCryptoFrame(
                cryptoPayload.data(), cryptoPayload.size());
            const bool rawCryptoValid = crypto.IsOk()
                && crypto.Value().consumedBytes == cryptoPayload.size();
            if (m_packetProtectionReady || !rawCryptoValid) {
                if (m_packetProtectionProvider == nullptr) {
                    return Fail(m_packetProtectionReady
                        ? QuicWireEngineError::PacketProtectionUnavailable
                        : QuicWireEngineError::InvalidDatagram);
                }
                plaintext.resize(m_options.maximumDatagramBytes);
                const auto* associatedDataBegin = protectedPacketBegin;
                const auto associatedDataSize = static_cast<std::size_t>(
                    header.payload.data() - associatedDataBegin);
                const auto protection = UnprotectPacket({
                    ProtectionLevel(header.type),
                    fullPacketNumber,
                    { associatedDataBegin, associatedDataSize },
                    header.payload
                }, plaintext);
                if (!protection.Succeeded() || protection.bytesWritten == 0
                    || protection.bytesWritten > plaintext.size()) {
                    return Fail(QuicWireEngineError::PacketProtectionUnavailable);
                }
                cryptoPayload = { plaintext.data(), protection.bytesWritten };
            }

            const auto dispatched = DispatchLongHeaderFrames(header.type, cryptoPayload);
            if (!dispatched.Succeeded()) return dispatched;
            if (!packetNumbers->Observe(fullPacketNumber).IsOk()) {
                return Fail(QuicWireEngineError::InvalidDatagram);
            }
            return dispatched;
        }

        QuicResult QuicWireEngine::HandleTimeout() {
            if (m_state == QuicState::Handshaking) {
                if (!m_started) return Fail(QuicWireEngineError::InvalidState);
                m_state = QuicState::Failed;
                m_lastError = ErrorCode(QuicWireEngineError::HandshakeTimeout);
                return {
                    QuicAction::ConnectionClose,
                    m_lastError,
                    {}
                };
            }
            if (m_state != QuicState::Active) return {};
            if (!m_started) return Fail(QuicWireEngineError::InvalidState);

            // 外部 scheduler 已确认 recovery deadline；Engine 不读取时钟。
            const auto expired = m_ackRecoveryLedger.CollectExpired(
                Time::SteadyTimePoint::max());
            if (!expired.IsOk()) return Fail(QuicWireEngineError::InvalidDatagram);
            QuicAction actions = QuicAction::None;
            for (const auto& packet : expired.Value()) {
                Address target = m_peer;
                const auto peerIt = m_retransmissionPeers.find(packet.packetNumber);
                if (peerIt != m_retransmissionPeers.end()) {
                    target = peerIt->second;
                    m_retransmissionPeers.erase(peerIt);
                }
                // 先释放旧包 reservation，避免重传时重复占用 credit。
                const auto lost = m_creditCongestionLedger.Lose(packet.packetNumber);
                if (!lost.IsOk()
                    && lost.GetStatus().Code() != StatusCode::NotFound) {
                    return Fail(QuicWireEngineError::InvalidDatagram);
                }
                std::uint64_t streamId = UINT64_MAX;
                if (lost.IsOk()) {
                    // 只有原包持有 stream reservation 时才恢复同一 stream。
                    const auto stream = ParseQuicStreamFrame(
                        packet.payload.data(), packet.payload.size());
                    if (!stream.IsOk()
                        || stream.Value().consumedBytes != packet.payload.size()) {
                        return Fail(QuicWireEngineError::InvalidDatagram);
                    }
                    streamId = stream.Value().streamId;
                }
                // 重用原始明文 frame，重新生成 caller-owned protected packet。
                const auto retransmitted = SendProtectedFrameTo(
                    packet.payload, target, streamId);
                if (!retransmitted.Succeeded()) return retransmitted;
                actions = actions | retransmitted.actions;
            }
            if (expired.Value().empty()) return {};
            return { actions, 0, kHandshakeTimer };
        }

        QuicResult QuicWireEngine::SendProtectedFrame(
            std::span<const std::uint8_t> frame,
            std::uint64_t streamId) {
            return SendProtectedFrameTo(frame, m_peer, streamId);
        }

        QuicResult QuicWireEngine::SendProtectedFrameTo(
            std::span<const std::uint8_t> frame,
            const Address& peer,
            std::uint64_t streamId) {
            if (m_state != QuicState::Active) return Fail(QuicWireEngineError::InvalidState);
            if (m_packetProtectionProvider == nullptr
                || m_packetHeaderProtectionProvider == nullptr) {
                return Fail(QuicWireEngineError::PacketProtectionUnavailable);
            }
            if (frame.empty() || frame.size() > m_options.maximumDatagramBytes) {
                return Fail(QuicWireEngineError::InvalidDatagram);
            }

            const auto packetNumber = EncodeQuicPacketNumber(m_nextPacketNumber);
            if (!packetNumber.IsOk()) return Fail(QuicWireEngineError::InvalidDatagram);

            const auto header = BuildQuicShortHeaderPacket({
                false,
                false,
                m_options.destinationConnectionId,
                { packetNumber.Value().Data(), packetNumber.Value().size },
                {}
            });
            if (!header.IsOk() || header.Value().size() >= m_options.maximumDatagramBytes) {
                return Fail(QuicWireEngineError::DatagramTooLarge);
            }

            std::vector<std::uint8_t> protectedPayload(
                m_options.maximumDatagramBytes - header.Value().size());
            const auto protection = ProtectPacket({
                QuicPacketProtectionLevel::OneRtt,
                m_nextPacketNumber,
                header.Value(),
                frame
            }, protectedPayload);
            if (!protection.Succeeded() || protection.bytesWritten == 0
                || protection.bytesWritten > protectedPayload.size()) {
                return Fail(QuicWireEngineError::PacketProtectionUnavailable);
            }

            const auto packet = BuildQuicShortHeaderPacket({
                false,
                false,
                m_options.destinationConnectionId,
                { packetNumber.Value().Data(), packetNumber.Value().size },
                { protectedPayload.data(), protection.bytesWritten }
            });
            if (!packet.IsOk() || packet.Value().size() > m_options.maximumDatagramBytes) {
                return Fail(QuicWireEngineError::DatagramTooLarge);
            }

            std::vector<std::uint8_t> protectedPacket(m_options.maximumDatagramBytes);
            const auto headerProtection =
                m_packetHeaderProtectionProvider->ProtectShortHeader(
                    {
                        QuicPacketProtectionLevel::OneRtt,
                        m_hasReceivedPacketNumber ? m_largestReceivedPacketNumber : 0,
                        m_options.destinationConnectionIdLength,
                        packet.Value()
                    },
                    protectedPacket);
            const QuicShortHeaderProtectionRequest headerRequest{
                QuicPacketProtectionLevel::OneRtt,
                m_hasReceivedPacketNumber ? m_largestReceivedPacketNumber : 0,
                m_options.destinationConnectionIdLength,
                packet.Value()
            };
            if (ValidateQuicShortHeaderProtectionResult(
                    headerRequest,
                    headerProtection,
                    std::span<const std::uint8_t>(
                        protectedPacket.data(), protectedPacket.size()))
                    != QuicPacketHeaderProtectionError::None
                || headerProtection.packetNumberLength != packetNumber.Value().size) {
                return Fail(QuicWireEngineError::InvalidDatagram);
            }

            const auto recoveryLimits = m_ackRecoveryLedger.Limits();
            const auto recoverySnapshot = m_ackRecoveryLedger.Snapshot();
            if (recoverySnapshot.retransmission.trackedPackets
                    >= recoveryLimits.retransmission.maxPackets
                || frame.size() > recoveryLimits.retransmission.maxPayloadBytes
                || recoverySnapshot.retransmission.trackedBytes
                    > recoveryLimits.retransmission.maxPayloadBytes - frame.size()) {
                return Fail(QuicWireEngineError::DatagramTooLarge);
            }

            bool creditReserved = false;
            if (streamId != UINT64_MAX) {
                const auto streamCredit = m_creditCongestionLedger.StreamCredit(streamId);
                if (streamCredit.IsOk()) {
                    const auto reserved = m_creditCongestionLedger.Reserve(
                        m_nextPacketNumber, streamId, frame.size());
                    if (!reserved.IsOk()
                        && reserved.GetStatus().Code() == StatusCode::ResourceExhausted) {
                        return {
                            QuicAction::None,
                            ErrorCode(QuicWireEngineError::FlowControlBlocked),
                            {}
                        };
                    }
                    if (!reserved.IsOk()) {
                        return Fail(QuicWireEngineError::DatagramTooLarge);
                    }
                    creditReserved = true;
                }
            }

            if (!peer.IsValid()) {
                if (creditReserved) {
                    (void)m_creditCongestionLedger.Lose(m_nextPacketNumber);
                }
                return Reject(QuicWireEngineError::InvalidState);
            }

            QuicActionMessage timer(QuicActionKind::ArmTimer);
            timer.peer = peer;
            timer.timerDelay = kHandshakeTimer;
            if (!EmitAction(std::move(timer))) {
                if (creditReserved) {
                    (void)m_creditCongestionLedger.Lose(m_nextPacketNumber);
                }
                return Fail(QuicWireEngineError::ActionRejected);
            }

            QuicActionMessage action(QuicActionKind::DatagramReady);
            action.peer = peer;
            action.payload = MakeBuffer({
                protectedPacket.data(), headerProtection.bytesWritten });
            if (!EmitAction(std::move(action))) {
                if (creditReserved) {
                    (void)m_creditCongestionLedger.Lose(m_nextPacketNumber);
                }
                return Fail(QuicWireEngineError::ActionRejected);
            }

            const std::vector<std::uint8_t> retransmissionPayload(
                frame.begin(), frame.end());
            const auto tracked = m_ackRecoveryLedger.TrackSent(
                m_nextPacketNumber,
                retransmissionPayload,
                Time::SteadyTimePoint::max());
            if (!tracked.IsOk()) return Fail(QuicWireEngineError::DatagramTooLarge);
            m_retransmissionPeers.emplace(m_nextPacketNumber, peer);
            ++m_nextPacketNumber;
            return { QuicAction::DatagramReady | QuicAction::ArmTimer,
                0, kHandshakeTimer };
        }

        QuicResult QuicWireEngine::SendStreamData(
            std::uint64_t streamId,
            Buffer&& plaintext) {
            if (m_state != QuicState::Active) return Fail(QuicWireEngineError::InvalidState);
            if (streamId >= (std::uint64_t{ 1 } << 62)
                || plaintext.ReadableBytes() > m_options.maximumDatagramBytes) {
                return Fail(QuicWireEngineError::InvalidDatagram);
            }
            const auto offsetIt = m_streamSendOffsets.find(streamId);
            const auto offset = offsetIt == m_streamSendOffsets.end()
                ? std::uint64_t{ 0 } : offsetIt->second;
            if (plaintext.ReadableBytes() >
                std::numeric_limits<std::uint64_t>::max() - offset) {
                return Fail(QuicWireEngineError::InvalidDatagram);
            }
            const auto frame = BuildQuicStreamFrame({
                streamId,
                offset,
                offset != 0,
                true,
                false,
                { plaintext.Peek(), plaintext.ReadableBytes() }
            });
            if (!frame.IsOk()) return Fail(QuicWireEngineError::InvalidDatagram);
            const auto result = SendProtectedFrame(frame.Value(), streamId);
            if (result.Succeeded()) {
                m_streamSendOffsets[streamId] = offset + plaintext.ReadableBytes();
                plaintext.RetrieveAll();
            }
            return result;
        }

        QuicResult QuicWireEngine::SendStreamFin(std::uint64_t streamId) {
            if (m_state != QuicState::Active) return Fail(QuicWireEngineError::InvalidState);
            const auto offsetIt = m_streamSendOffsets.find(streamId);
            const auto offset = offsetIt == m_streamSendOffsets.end()
                ? std::uint64_t{ 0 } : offsetIt->second;
            const auto frame = BuildQuicStreamFrame({
                streamId,
                offset,
                offset != 0,
                true,
                true,
                {}
            });
            if (!frame.IsOk()) return Fail(QuicWireEngineError::InvalidDatagram);
            return SendProtectedFrame(frame.Value());
        }

        QuicResult QuicWireEngine::ResetStream(
            std::uint64_t streamId,
            std::uint64_t errorCode) {
            if (m_state != QuicState::Active) return Fail(QuicWireEngineError::InvalidState);
            const auto frame = BuildQuicResetStreamFrame({ streamId, errorCode, 0, 0 });
            if (!frame.IsOk()) return Fail(QuicWireEngineError::InvalidDatagram);
            return SendProtectedFrame(frame.Value());
        }

        QuicResult QuicWireEngine::StopSending(
            std::uint64_t streamId,
            std::uint64_t errorCode) {
            if (m_state != QuicState::Active) return Fail(QuicWireEngineError::InvalidState);
            const auto frame = BuildQuicStopSendingFrame({ streamId, errorCode, 0 });
            if (!frame.IsOk()) return Fail(QuicWireEngineError::InvalidDatagram);
            return SendProtectedFrame(frame.Value());
        }

        QuicResult QuicWireEngine::Close(std::uint64_t errorCode) {
            if (m_state == QuicState::Closed) return {};
            if (m_actionSink == nullptr) {
                return Fail(QuicWireEngineError::MissingActionSink);
            }
            QuicActionMessage close(
                QuicActionKind::CloseConnection, 0, errorCode);
            if (!EmitAction(std::move(close))) {
                return Fail(QuicWireEngineError::ActionRejected);
            }
            m_state = QuicState::Closed;
            EmitEvent(QuicEventKind::ConnectionClose, 0, Buffer(0), errorCode);
            return { QuicAction::ConnectionClose, 0, {} };
        }

        QuicState QuicWireEngine::State() const noexcept {
            return m_state;
        }

        QuicEngineSnapshot QuicWireEngine::Snapshot() const noexcept {
            const auto ack = m_ackRecoveryLedger.Snapshot();
            const auto credit = m_creditCongestionLedger.Snapshot();
            const auto path = m_pathMigrationLedger.Snapshot();
            QuicEngineSnapshot snapshot;
            snapshot.state = m_state;
            snapshot.tlsVersion = m_tlsVersion;
            snapshot.pathState = m_pathState;
            snapshot.handshakeComplete = m_state == QuicState::Active;
            snapshot.packetProtectionReady = m_packetProtectionReady;
            snapshot.activeStreams = m_streamReceiveStates.size();
            snapshot.pendingDatagrams = m_pendingDatagrams;
            snapshot.receivedDatagrams = m_receivedDatagrams;
            snapshot.sentDatagrams = m_sentDatagrams;
            snapshot.lastError = m_lastError;
            snapshot.observedAckFrames = ack.acknowledgement.observedFrames;
            snapshot.largestAcknowledgedPacket =
                ack.acknowledgement.largestObserved;
            snapshot.pendingRetransmissions = ack.retransmission.trackedPackets;
            snapshot.peerConnectionCredit = credit.connection.limit;
            snapshot.reservedConnectionCredit = credit.connection.reserved;
            snapshot.trackedStreamCredits = credit.trackedStreams;
            snapshot.congestionWindowBytes = credit.congestion.windowBytes;
            snapshot.congestionInFlightBytes = credit.congestion.inFlightBytes;
            snapshot.pathChallengePending = path.probe.pending;
            snapshot.hasConnectionIdSequence =
                path.connectionId.hasIssuedSequence;
            snapshot.highestConnectionIdSequence =
                path.connectionId.highestIssuedSequence;
            snapshot.recoveryTimerArmed = snapshot.pendingRetransmissions != 0;
            snapshot.recoveryTimerDelay = snapshot.recoveryTimerArmed
                ? kHandshakeTimer : std::chrono::milliseconds(0);
            const auto initialNumbers = m_initialPacketNumbers.Snapshot();
            const auto handshakeNumbers = m_handshakePacketNumbers.Snapshot();
            const auto zeroRttNumbers = m_zeroRttPacketNumbers.Snapshot();
            snapshot.hasInitialPacketNumber = initialNumbers.hasLargestReceived;
            snapshot.largestInitialPacketNumber = initialNumbers.largestReceived;
            snapshot.hasHandshakePacketNumber = handshakeNumbers.hasLargestReceived;
            snapshot.largestHandshakePacketNumber = handshakeNumbers.largestReceived;
            snapshot.hasZeroRttPacketNumber = zeroRttNumbers.hasLargestReceived;
            snapshot.largestZeroRttPacketNumber = zeroRttNumbers.largestReceived;
            return snapshot;
        }

        const char* QuicWireEngine::NegotiatedProtocol() const noexcept {
            return m_state == QuicState::Active
                ? m_negotiatedProtocol.c_str() : "";
        }

        QuicTlsVersion QuicWireEngine::NegotiatedTlsVersion() const noexcept {
            return m_tlsVersion;
        }

        QuicResult QuicWireEngine::Fail(QuicWireEngineError error) noexcept {
            m_state = QuicState::Failed;
            m_lastError = ErrorCode(error);
            return { QuicAction::None, m_lastError, {} };
        }

        QuicResult QuicWireEngine::Reject(QuicWireEngineError error) noexcept {
            return { QuicAction::None, ErrorCode(error), {} };
        }

        void QuicWireEngine::ForgetAcknowledgedPacketPeers(
            const QuicAckFrame& frame) noexcept {
            for (const auto& range : frame.ranges) {
                auto it = m_retransmissionPeers.lower_bound(range.smallest);
                while (it != m_retransmissionPeers.end()
                    && it->first <= range.largest) {
                    it = m_retransmissionPeers.erase(it);
                }
            }
        }

        QuicActionDelivery QuicWireEngine::Submit(QuicActionMessage&& action) noexcept {
            const auto actionId = action.actionId;
            if (m_actionSink == nullptr) {
                return {
                    QuicActionDeliveryState::Rejected,
                    QuicActionDeliveryError::TransportUnavailable,
                    actionId
                };
            }
            return m_actionSink->Submit(std::move(action));
        }

        bool QuicWireEngine::EmitAction(QuicActionMessage&& action) noexcept {
            if (action.actionId == 0) action.actionId = m_nextActionId++;
            const auto actionId = action.actionId;
            const auto kind = action.kind;
            const auto delivery = Submit(std::move(action));
            if (delivery.Rejected() || !delivery.Matches(actionId)) return false;
            if (kind == QuicActionKind::DatagramReady) {
                ++m_sentDatagrams;
                if (delivery.Deferred()) ++m_pendingDatagrams;
            }
            return true;
        }

        void QuicWireEngine::EmitEvent(
            QuicEventKind kind,
            std::uint64_t streamId,
            Buffer&& payload,
            std::uint64_t errorCode,
            bool applicationError) noexcept {
            if (m_eventObserver == nullptr) return;
            QuicStreamEvent event;
            event.kind = kind;
            event.streamId = streamId;
            event.errorCode = errorCode;
            event.applicationError = applicationError;
            event.peer = m_peer;
            event.payload = std::move(payload);
            m_eventObserver->Observe(std::move(event));
        }

        QuicResult QuicWireEngine::BuildAndEmitInitial() {
            if (!m_protectedDatagram.empty()) {
                QuicActionMessage datagram(QuicActionKind::DatagramReady);
                datagram.payload = MakeBuffer(m_protectedDatagram);
                if (!EmitAction(std::move(datagram))) {
                    return Fail(QuicWireEngineError::ActionRejected);
                }
                return { QuicAction::DatagramReady, 0, {} };
            }

            const auto crypto = BuildQuicCryptoFrame({
                0,
                std::span<const std::uint8_t>(
                    m_tlsHandshakeData.data(), m_tlsHandshakeData.size())
            });
            if (!crypto.IsOk()) return Fail(QuicWireEngineError::DatagramTooLarge);

            const std::array<std::uint8_t, 1> packetNumber{ 0 };
            const auto packet = BuildQuicLongHeaderPacket({
                QuicLongPacketType::Initial,
                1,
                {},
                {},
                {},
                packetNumber,
                crypto.Value(),
                {}
            });
            if (!packet.IsOk() || packet.Value().size() > m_options.maximumDatagramBytes) {
                return Fail(QuicWireEngineError::DatagramTooLarge);
            }

            QuicActionMessage datagram(QuicActionKind::DatagramReady);
            datagram.payload = MakeBuffer(packet.Value());
            if (!EmitAction(std::move(datagram))) {
                return Fail(QuicWireEngineError::ActionRejected);
            }
            return { QuicAction::DatagramReady, 0, {} };
        }

        std::unique_ptr<QuicEngine> CreateQuicWireEngine(
            const QuicEngineOptions& options) noexcept {
            if (!IsValidQuicEngineOptions(options)) return {};
            try {
                return std::make_unique<QuicWireEngine>(options);
            }
            catch (...) {
                return {};
            }
        }
    }
}
