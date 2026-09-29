#include <LikesProgram/Quic/QuicCryptoFrame.hpp>
#include <LikesProgram/Quic/QuicDatagramFrame.hpp>
#include <LikesProgram/Quic/QuicFrameType.hpp>
#include <LikesProgram/Quic/QuicHandshakeDoneFrame.hpp>
#include <LikesProgram/Quic/QuicNewTokenFrame.hpp>
#include <LikesProgram/Quic/QuicPingFrame.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
    using namespace LikesProgram::Quic;

    struct FrameObservation {
        QuicFrameType kind = QuicFrameType::Unknown;
        std::size_t offset = 0;
        std::size_t consumedBytes = 0;
    };

    void Require(bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    }

    bool Append(std::vector<std::uint8_t>& destination,
        const LikesProgram::Result<std::vector<std::uint8_t>>& encoded) {
        if (!encoded.IsOk()) return false;
        const auto& bytes = encoded.Value();
        destination.insert(destination.end(), bytes.begin(), bytes.end());
        return true;
    }

    bool DispatchSequence(std::span<const std::uint8_t> bytes,
        std::vector<FrameObservation>& observations) {
        observations.clear();
        std::vector<FrameObservation> pending;
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const auto type = ParseQuicFrameType(
                bytes.data() + offset, bytes.size() - offset);
            if (!type.IsOk()) return false;

            std::size_t consumedBytes = 0;
            switch (type.Value().kind) {
            case QuicFrameType::Ping: {
                const auto parsed = ParseQuicPingFrame(
                    bytes.data() + offset, bytes.size() - offset);
                if (!parsed.IsOk()) return false;
                consumedBytes = parsed.Value().consumedBytes;
                break;
            }
            case QuicFrameType::Crypto: {
                const auto parsed = ParseQuicCryptoFrame(
                    bytes.data() + offset, bytes.size() - offset);
                if (!parsed.IsOk()) return false;
                consumedBytes = parsed.Value().consumedBytes;
                break;
            }
            case QuicFrameType::NewToken: {
                const auto parsed = ParseQuicNewTokenFrame(
                    bytes.data() + offset, bytes.size() - offset);
                if (!parsed.IsOk()) return false;
                consumedBytes = parsed.Value().consumedBytes;
                break;
            }
            case QuicFrameType::HandshakeDone: {
                const auto parsed = ParseQuicHandshakeDoneFrame(
                    bytes.data() + offset, bytes.size() - offset);
                if (!parsed.IsOk()) return false;
                consumedBytes = parsed.Value().consumedBytes;
                break;
            }
            case QuicFrameType::Datagram: {
                if (type.Value().value != 0x31) return false;
                const auto parsed = ParseQuicDatagramFrame(
                    bytes.data() + offset, bytes.size() - offset);
                if (!parsed.IsOk() || !parsed.Value().hasLength) return false;
                consumedBytes = parsed.Value().consumedBytes;
                break;
            }
            default:
                return false;
            }

            if (consumedBytes < type.Value().encodedBytes
                || consumedBytes > bytes.size() - offset) {
                return false;
            }
            pending.push_back({ type.Value().kind, offset, consumedBytes });
            offset += consumedBytes;
        }
        observations = std::move(pending);
        return true;
    }
}

int Run() {
    using namespace LikesProgram::Quic;
    const std::array<std::uint8_t, 2> cryptoData{ 0x10, 0x11 };
    const std::array<std::uint8_t, 3> token{ 0x20, 0x21, 0x22 };
    const std::array<std::uint8_t, 3> datagramData{ 0x30, 0x31, 0x32 };

    std::vector<std::uint8_t> sequence;
    Require(Append(sequence, BuildQuicPingFrame()), "PING should build");
    Require(Append(sequence, BuildQuicCryptoFrame({ 4, cryptoData })),
        "CRYPTO should build");
    Require(Append(sequence, BuildQuicNewTokenFrame({ token })),
        "NEW_TOKEN should build");
    Require(Append(sequence, BuildQuicHandshakeDoneFrame()),
        "HANDSHAKE_DONE should build");
    Require(Append(sequence, BuildQuicDatagramFrame(true, datagramData)),
        "length-bearing DATAGRAM should build");

    std::vector<FrameObservation> observations;
    Require(DispatchSequence(sequence, observations)
        && observations.size() == 5,
        "known frame sequence should dispatch completely");
    const std::array<QuicFrameType, 5> expectedKinds{
        QuicFrameType::Ping, QuicFrameType::Crypto, QuicFrameType::NewToken,
        QuicFrameType::HandshakeDone, QuicFrameType::Datagram };
    std::size_t expectedOffset = 0;
    for (std::size_t index = 0; index < observations.size(); ++index) {
        Require(observations[index].kind == expectedKinds[index]
            && observations[index].offset == expectedOffset
            && observations[index].consumedBytes > 0,
            "dispatch observation coordinates should be monotonic");
        expectedOffset += observations[index].consumedBytes;
    }
    Require(expectedOffset == sequence.size(),
        "dispatch must consume the complete sequence");

    const std::array<std::uint8_t, 1> trailing{ 0xA5 };
    auto pingWithTrailing = BuildQuicPingFrame().Value();
    pingWithTrailing.insert(pingWithTrailing.end(), trailing.begin(), trailing.end());
    const auto parsedPing = ParseQuicPingFrame(
        pingWithTrailing.data(), pingWithTrailing.size());
    Require(parsedPing.IsOk() && parsedPing.Value().consumedBytes == 1,
        "individual parser must preserve trailing boundary");

    const auto oldObservations = observations;
    auto unknown = sequence;
    const auto extension = EncodeQuicVarInt(0x2a);
    Require(extension.IsOk(), "unknown extension type should encode");
    unknown.insert(unknown.begin() + static_cast<std::ptrdiff_t>(
        observations[1].offset), extension.Value().storage.begin(),
        extension.Value().storage.begin() + extension.Value().size);
    Require(!DispatchSequence(unknown, observations) && observations.empty(),
        "unknown extension must reject without partial observations");
    observations = oldObservations;

    auto truncated = sequence;
    truncated.pop_back();
    Require(!DispatchSequence(truncated, observations) && observations.empty(),
        "truncated payload must reject without partial observations");
    observations = oldObservations;

    const std::array<std::uint8_t, 1> malformed{ 0x40 };
    Require(!DispatchSequence(malformed, observations) && observations.empty(),
        "truncated frame-type varint must reject without partial observations");
    observations = oldObservations;

    const auto lengthless = BuildQuicDatagramFrame(false, datagramData);
    Require(lengthless.IsOk()
        && !DispatchSequence(lengthless.Value(), observations)
        && observations.empty(),
        "lengthless DATAGRAM must not erase the enclosing sequence boundary");

    std::cout << "passed=true"
              << " sequence_roundtrip=true"
              << " coordinates=true"
              << " trailing_boundary=true"
              << " unknown_rejected=true"
              << " truncated_rejected=true"
              << " malformed_rejected=true"
              << " state_preserved=true\n";
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
