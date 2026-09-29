#include <LikesProgram/Quic/QuicEngineFactory.hpp>

#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>

namespace {
    using LikesProgram::Net::Address;
    using LikesProgram::Net::Buffer;
    using LikesProgram::Quic::QuicEngine;
    using LikesProgram::Quic::QuicEngineSnapshot;
    using LikesProgram::Quic::QuicEngineOptions;
    using LikesProgram::Quic::QuicPacketProtectionError;
    using LikesProgram::Quic::QuicPacketProtectionLevel;
    using LikesProgram::Quic::QuicPacketProtectionProvider;
    using LikesProgram::Quic::QuicPacketProtectionRequest;
    using LikesProgram::Quic::QuicPacketProtectionResult;
    using LikesProgram::Quic::QuicResult;
    using LikesProgram::Quic::QuicState;
    using LikesProgram::Quic::QuicTlsHandshakeResult;
    using LikesProgram::Quic::QuicTlsVersion;

    class MarkerProvider final : public QuicPacketProtectionProvider {
    public:
        QuicPacketProtectionResult Protect(
            const QuicPacketProtectionRequest& request,
            std::span<std::uint8_t> output) noexcept override {
            protectCalls++;
            Capture(request);
            const auto validation = LikesProgram::Quic::ValidateQuicPacketProtectionRequest(
                request, std::span<const std::uint8_t>(output.data(), output.size()));
            if (validation != QuicPacketProtectionError::None) {
                return { validation, 0 };
            }
            if (failProtect) return { QuicPacketProtectionError::AuthenticationFailed, 0 };
            for (std::size_t i = 0; i < request.payload.size(); ++i) {
                output[i] = static_cast<std::uint8_t>(request.payload[i] ^ 0xA5);
            }
            return { QuicPacketProtectionError::None, request.payload.size() };
        }

        QuicPacketProtectionResult Unprotect(
            const QuicPacketProtectionRequest& request,
            std::span<std::uint8_t> output) noexcept override {
            unprotectCalls++;
            Capture(request);
            const auto validation = LikesProgram::Quic::ValidateQuicPacketProtectionRequest(
                request, std::span<const std::uint8_t>(output.data(), output.size()));
            if (validation != QuicPacketProtectionError::None) {
                return { validation, 0 };
            }
            if (failUnprotect) {
                return { QuicPacketProtectionError::AuthenticationFailed, 0 };
            }
            for (std::size_t i = 0; i < request.payload.size(); ++i) {
                output[i] = static_cast<std::uint8_t>(request.payload[i] ^ 0xA5);
            }
            return { QuicPacketProtectionError::None, request.payload.size() };
        }

        void Capture(const QuicPacketProtectionRequest& request) noexcept {
            lastLevel = request.level;
            lastPacketNumber = request.packetNumber;
            lastAssociatedDataBytes = request.associatedData.size();
            lastPayloadBytes = request.payload.size();
        }

        int protectCalls = 0;
        int unprotectCalls = 0;
        bool failProtect = false;
        bool failUnprotect = false;
        QuicPacketProtectionLevel lastLevel = QuicPacketProtectionLevel::Initial;
        std::uint64_t lastPacketNumber = 0;
        std::size_t lastAssociatedDataBytes = 0;
        std::size_t lastPayloadBytes = 0;
    };

    class ProviderEngine final : public QuicEngine {
    public:
        void SetActionSink(LikesProgram::Quic::QuicActionSink*) noexcept override {}
        void SetEventObserver(LikesProgram::Quic::QuicEventObserver*) noexcept override {}

        void SetPacketProtectionProvider(QuicPacketProtectionProvider* provider) noexcept override {
            m_provider = provider;
        }

        QuicPacketProtectionResult ProtectPacket(
            const QuicPacketProtectionRequest& request,
            std::span<std::uint8_t> output) noexcept override {
            if (m_provider == nullptr) {
                return { QuicPacketProtectionError::NotReady, 0 };
            }
            return m_provider->Protect(request, output);
        }

        QuicPacketProtectionResult UnprotectPacket(
            const QuicPacketProtectionRequest& request,
            std::span<std::uint8_t> output) noexcept override {
            if (m_provider == nullptr) {
                return { QuicPacketProtectionError::NotReady, 0 };
            }
            return m_provider->Unprotect(request, output);
        }

        QuicResult StartHandshake() override { return {}; }
        QuicResult ProvideTlsHandshakeResult(const QuicTlsHandshakeResult&) override {
            return {};
        }
        QuicResult ConsumeDatagram(const Address&, Buffer&& datagram) override {
            datagram.RetrieveAll();
            return {};
        }
        QuicResult HandleTimeout() override { return {}; }
        QuicResult SendStreamData(std::uint64_t, Buffer&&) override { return {}; }
        QuicResult SendStreamFin(std::uint64_t) override { return {}; }
        QuicResult ResetStream(std::uint64_t, std::uint64_t) override { return {}; }
        QuicResult StopSending(std::uint64_t, std::uint64_t) override { return {}; }
        QuicResult Close(std::uint64_t) override { return {}; }
        QuicState State() const noexcept override { return QuicState::Handshaking; }
        QuicEngineSnapshot Snapshot() const noexcept override {
            return {};
        }
        const char* NegotiatedProtocol() const noexcept override { return ""; }
        QuicTlsVersion NegotiatedTlsVersion() const noexcept override {
            return QuicTlsVersion::Unknown;
        }

    private:
        QuicPacketProtectionProvider* m_provider = nullptr;
    };

    bool IsError(const QuicPacketProtectionResult& result,
        QuicPacketProtectionError error) {
        return result.error == error && result.bytesWritten == 0;
    }
}

int main() {
    using namespace LikesProgram::Quic;

    QuicEngineFactory factory([](const QuicEngineOptions&) {
        return std::make_unique<ProviderEngine>();
    });
    auto engine = factory.Create({});
    if (!engine) return 1;

    const std::array<std::uint8_t, 2> associatedData{ 0x01, 0x02 };
    const std::array<std::uint8_t, 3> payload{ 0x10, 0x20, 0x30 };
    const QuicPacketProtectionRequest request{
        QuicPacketProtectionLevel::Handshake, 17, associatedData, payload };
    std::array<std::uint8_t, 3> output{ 0xCC, 0xCC, 0xCC };
    if (!IsError(engine->ProtectPacket(request, output),
            QuicPacketProtectionError::NotReady)
        || output != std::array<std::uint8_t, 3>{ 0xCC, 0xCC, 0xCC }) return 2;

    MarkerProvider provider;
    engine->SetPacketProtectionProvider(&provider);
    const auto protectedResult = engine->ProtectPacket(request, output);
    if (!protectedResult.Succeeded()
        || protectedResult.bytesWritten != payload.size()
        || output != std::array<std::uint8_t, 3>{ 0xB5, 0x85, 0x95 }
        || provider.protectCalls != 1
        || provider.lastLevel != QuicPacketProtectionLevel::Handshake
        || provider.lastPacketNumber != 17
        || provider.lastAssociatedDataBytes != 2
        || provider.lastPayloadBytes != 3) return 3;

    std::array<std::uint8_t, 3> plaintext{ 0, 0, 0 };
    const QuicPacketProtectionRequest protectedRequest{
        request.level, request.packetNumber, request.associatedData, output };
    const auto unprotectedResult = engine->UnprotectPacket(protectedRequest, plaintext);
    if (!unprotectedResult.Succeeded()
        || unprotectedResult.bytesWritten != payload.size()
        || plaintext != payload
        || provider.unprotectCalls != 1) return 4;

    std::array<std::uint8_t, 2> tooSmall{ 0xCC, 0xCC };
    if (!IsError(engine->ProtectPacket(request, tooSmall),
            QuicPacketProtectionError::OutputTooSmall)
        || tooSmall != std::array<std::uint8_t, 2>{ 0xCC, 0xCC }) return 5;

    const QuicPacketProtectionRequest invalidLevel{
        static_cast<QuicPacketProtectionLevel>(99), 17, associatedData, payload };
    if (!IsError(engine->ProtectPacket(invalidLevel, output),
            QuicPacketProtectionError::UnsupportedLevel)) return 6;

    provider.failUnprotect = true;
    plaintext = { 0xCC, 0xCC, 0xCC };
    if (!IsError(engine->UnprotectPacket(protectedRequest, plaintext),
            QuicPacketProtectionError::AuthenticationFailed)
        || plaintext != std::array<std::uint8_t, 3>{ 0xCC, 0xCC, 0xCC }) return 7;
    provider.failUnprotect = false;

    MarkerProvider replacement;
    engine->SetPacketProtectionProvider(&replacement);
    if (!engine->ProtectPacket(request, output).Succeeded()
        || replacement.protectCalls != 1
        || provider.protectCalls != 3) return 8;
    engine->SetPacketProtectionProvider(nullptr);
    if (!IsError(engine->UnprotectPacket(protectedRequest, plaintext),
            QuicPacketProtectionError::NotReady)) return 9;

    std::cout << "passed=true"
              << " protect_calls=3"
              << " unprotect_calls=2"
              << " spans_forwarded=true"
              << " errors_preserved=true"
              << " provider_replacement=true"
              << " keys_external=true\n";
    return 0;
}
