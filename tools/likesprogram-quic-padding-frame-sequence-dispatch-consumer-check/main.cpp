#include <LikesProgram/Quic/QuicCryptoFrame.hpp>
#include <LikesProgram/Quic/QuicFrameType.hpp>
#include <LikesProgram/Quic/QuicPingFrame.hpp>
#include <LikesProgram/Quic/QuicStreamFrame.hpp>

#include <array>
#include <cstddef>
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

        bool operator==(const FrameObservation&) const = default;
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
        std::vector<FrameObservation> pending;
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const auto type = ParseQuicFrameType(
                bytes.data() + offset, bytes.size() - offset);
            if (!type.IsOk()) return false;

            std::size_t consumedBytes = 0;
            switch (type.Value().kind) {
            case QuicFrameType::Padding:
                if (type.Value().value != 0) return false;
                consumedBytes = type.Value().encodedBytes;
                break;
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
            case QuicFrameType::Stream: {
                if ((type.Value().value & 0x02) == 0) return false;
                const auto parsed = ParseQuicStreamFrame(
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
    const std::array<std::uint8_t, 2> cryptoData{ 0x40, 0x41 };
    const std::array<std::uint8_t, 2> streamData{ 0x50, 0x51 };
    const QuicStreamFrameBuildOptions stream{
        3, 1, true, true, false, streamData };

    std::vector<std::uint8_t> sequence{ 0x00, 0x00, 0x00 };
    Require(Append(sequence, BuildQuicPingFrame()), "PING should build");
    Require(Append(sequence, BuildQuicCryptoFrame({ 0, cryptoData })),
        "CRYPTO should build");
    Require(Append(sequence, BuildQuicStreamFrame(stream)),
        "STREAM should build");
    sequence.push_back(0x00);

    std::vector<FrameObservation> observations;
    Require(DispatchSequence(sequence, observations)
        && observations.size() == 7,
        "padding and known frames should dispatch completely");
    const std::array<QuicFrameType, 7> expectedKinds{
        QuicFrameType::Padding, QuicFrameType::Padding,
        QuicFrameType::Padding, QuicFrameType::Ping,
        QuicFrameType::Crypto, QuicFrameType::Stream,
        QuicFrameType::Padding };
    std::size_t expectedOffset = 0;
    for (std::size_t index = 0; index < observations.size(); ++index) {
        Require(observations[index].kind == expectedKinds[index]
            && observations[index].offset == expectedOffset
            && observations[index].consumedBytes > 0,
            "padding/frame coordinates should be monotonic");
        expectedOffset += observations[index].consumedBytes;
    }
    Require(expectedOffset == sequence.size()
        && observations[0].consumedBytes == 1
        && observations[6].offset + observations[6].consumedBytes
            == sequence.size(),
        "padding must retain one-byte boundaries and complete tail");

    const auto baseline = observations;
    auto unknown = sequence;
    unknown.insert(unknown.begin() + 1, 0x2A);
    Require(!DispatchSequence(unknown, observations)
        && observations == baseline,
        "unknown extension among padding must reject atomically");

    auto truncated = sequence;
    truncated.pop_back();
    truncated.pop_back();
    Require(!DispatchSequence(truncated, observations)
        && observations == baseline,
        "truncated STREAM tail must reject atomically");

    const std::array<std::uint8_t, 1> malformedType{ 0x40 };
    Require(!DispatchSequence(malformedType, observations)
        && observations == baseline,
        "malformed frame type must reject atomically");

    std::cout << "passed=true"
              << " padding_boundaries=true"
              << " known_dispatch=true"
              << " coordinates=true"
              << " trailing_padding=true"
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
