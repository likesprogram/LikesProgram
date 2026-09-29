#pragma once

#include <LikesProgram/Quic/system/LikesProgramQuicExport.hpp>
#include <LikesProgram/Quic/QuicAckRecoveryLedger.hpp>
#include <LikesProgram/Quic/QuicCreditCongestionLedger.hpp>
#include <LikesProgram/Quic/QuicEngine.hpp>
#include <LikesProgram/Quic/QuicLongHeader.hpp>
#include <LikesProgram/Quic/QuicPacketNumberSpace.hpp>
#include <LikesProgram/Quic/QuicPathMigrationLedger.hpp>
#include <LikesProgram/Quic/QuicStreamFrame.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace LikesProgram {
    namespace Quic {
        // Errors owned by the production contract slice. They identify state
        // and transport-boundary failures; TLS and packet cryptography remain
        // caller-owned through QuicTlsHandshakeResult and the provider API.
        enum class QuicWireEngineError : int {
            None = 0,
            InvalidOptions = 2001,
            MissingActionSink = 2002,
            ActionRejected = 2003,
            InvalidState = 2004,
            DatagramTooLarge = 2005,
            InvalidDatagram = 2006,
            PacketProtectionUnavailable = 2007,
            HandshakeTimeout = 2008,
            UnsupportedPacket = 2009,
            StreamUnsupported = 2010,
            FlowControlBlocked = 2011
        };

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4251)
#endif

        // Stateful QUIC handshake/wire-boundary core. TLS, key schedule, packet
        // cryptography and UDP I/O remain caller-owned.
        class LIKESPROGRAM_QUIC_API QuicWireEngine final : public QuicEngine {
        public:
            explicit QuicWireEngine(const QuicEngineOptions& options);
            ~QuicWireEngine() override;

            QuicWireEngine(const QuicWireEngine&) = delete;
            QuicWireEngine& operator=(const QuicWireEngine&) = delete;

            // Supplies opaque CRYPTO bytes produced by the caller's TLS 1.3
            // implementation before the client Initial is emitted.
            QuicResult QueueTlsHandshakeData(Buffer&& data);
            // Supplies a complete caller-owned protected QUIC datagram. Before
            // handshake start it is queued as the client Initial; during
            // handshaking or while Active it is submitted immediately as a
            // DatagramReady action.
            QuicResult QueueProtectedDatagram(Buffer&& datagram);

            void SetActionSink(QuicActionSink* sink) noexcept override;
            void SetEventObserver(QuicEventObserver* observer) noexcept override;
            void SetPacketProtectionProvider(
                QuicPacketProtectionProvider* provider) noexcept override;
            QuicPacketProtectionResult ProtectPacket(
                const QuicPacketProtectionRequest& request,
                std::span<std::uint8_t> output) noexcept override;
            QuicPacketProtectionResult UnprotectPacket(
                const QuicPacketProtectionRequest& request,
                std::span<std::uint8_t> output) noexcept override;
            QuicResult StartHandshake() override;
            QuicResult ProvideTlsHandshakeResult(
                const QuicTlsHandshakeResult& result) override;
            Result<void> ApplyPeerFlowControl(
                const QuicFlowControlFrame& frame) override;
            QuicResult BeginPathValidation(
                const Address& candidate,
                const std::array<std::uint8_t, 8>& token) override;
            QuicResult ConsumeDatagram(
                const Address& peer,
                Buffer&& datagram) override;
            QuicResult HandleTimeout() override;
            QuicResult SendStreamData(
                std::uint64_t streamId,
                Buffer&& plaintext) override;
            QuicResult SendStreamFin(std::uint64_t streamId) override;
            QuicResult ResetStream(
                std::uint64_t streamId,
                std::uint64_t errorCode) override;
            QuicResult StopSending(
                std::uint64_t streamId,
                std::uint64_t errorCode) override;
            QuicResult Close(std::uint64_t errorCode = 0) override;
            QuicState State() const noexcept override;
            QuicEngineSnapshot Snapshot() const noexcept override;
            const char* NegotiatedProtocol() const noexcept override;
            QuicTlsVersion NegotiatedTlsVersion() const noexcept override;

        private:
            QuicResult Fail(QuicWireEngineError error) noexcept;
            QuicResult Reject(QuicWireEngineError error) noexcept;
            QuicActionDelivery Submit(QuicActionMessage&& action) noexcept;
            bool EmitAction(QuicActionMessage&& action) noexcept;
            void EmitEvent(
                QuicEventKind kind,
                std::uint64_t streamId = 0,
                Buffer&& payload = Buffer(0),
                std::uint64_t errorCode = 0,
                bool applicationError = false) noexcept;
            QuicResult BuildAndEmitInitial();
            QuicResult ReassembleStreamFrame(
                const QuicStreamFrame& frame);
            QuicResult DispatchOneRttFrames(
                const Address& peer,
                std::span<const std::uint8_t> plaintext);
            QuicResult DispatchLongHeaderFrames(
                QuicLongPacketType packetType,
                std::span<const std::uint8_t> plaintext);
            QuicResult SendProtectedFrame(
                std::span<const std::uint8_t> frame,
                std::uint64_t streamId = UINT64_MAX);
            QuicResult SendProtectedFrameTo(
                std::span<const std::uint8_t> frame,
                const Address& peer,
                std::uint64_t streamId = UINT64_MAX);
            void ForgetAcknowledgedPacketPeers(const QuicAckFrame& frame) noexcept;

            struct PendingStreamFrame {
                std::vector<std::uint8_t> data;
                bool fin = false;
            };

            struct StreamReceiveState {
                std::uint64_t nextOffset = 0;
                std::size_t pendingBytes = 0;
                bool finished = false;
                std::map<std::uint64_t, PendingStreamFrame> pending;
            };

            QuicEngineOptions m_options;
            std::string m_applicationProtocol;
            QuicState m_state = QuicState::Handshaking;
            QuicTlsVersion m_tlsVersion = QuicTlsVersion::Unknown;
            QuicPathState m_pathState = QuicPathState::Unknown;
            QuicActionSink* m_actionSink = nullptr;
            QuicEventObserver* m_eventObserver = nullptr;
            QuicPacketProtectionProvider* m_packetProtectionProvider = nullptr;
            QuicPacketHeaderProtectionProvider* m_packetHeaderProtectionProvider = nullptr;
            std::vector<std::uint8_t> m_tlsHandshakeData;
            std::vector<std::uint8_t> m_protectedDatagram;
            std::string m_negotiatedProtocol;
            Address m_peer;
            Address m_candidatePeer;
            bool m_hasCandidatePeer = false;
            bool m_started = false;
            bool m_packetProtectionReady = false;
            std::size_t m_pendingDatagrams = 0;
            std::uint64_t m_nextActionId = 1;
            std::uint64_t m_receivedDatagrams = 0;
            std::uint64_t m_sentDatagrams = 0;
            std::uint64_t m_nextPacketNumber = 0;
            std::uint64_t m_largestReceivedPacketNumber = 0;
            bool m_hasReceivedPacketNumber = false;
            std::map<std::uint64_t, StreamReceiveState> m_streamReceiveStates;
            // Caller actions append contiguous DATA at the next stream offset.
            std::map<std::uint64_t, std::uint64_t> m_streamSendOffsets;
            // Retains the original destination for every recovery packet so
            // timeout retransmission cannot redirect traffic across paths.
            std::map<std::uint64_t, Address> m_retransmissionPeers;
            QuicAckRecoveryLedger m_ackRecoveryLedger;
            QuicCreditCongestionLedger m_creditCongestionLedger;
            QuicPathMigrationLedger m_pathMigrationLedger;
            QuicPacketNumberSpace m_initialPacketNumbers;
            QuicPacketNumberSpace m_handshakePacketNumbers;
            QuicPacketNumberSpace m_zeroRttPacketNumbers;
            int m_lastError = 0;
        };

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

        // Convenience factory for the production boundary core. It returns
        // null for invalid options and never loads a TLS/QUIC implementation.
        LIKESPROGRAM_QUIC_API std::unique_ptr<QuicEngine>
            CreateQuicWireEngine(const QuicEngineOptions& options) noexcept;
    }
}
