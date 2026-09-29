#include <LikesProgram/Quic/QuicFrameType.hpp>
#include <LikesProgram/Quic/QuicStreamControlFrame.hpp>
#include <LikesProgram/Quic/QuicStreamFrame.hpp>

#include <algorithm>
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
    };

    void Require(bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    }

    bool SameBytes(std::span<const std::uint8_t> left,
        std::span<const std::uint8_t> right) {
        return left.size() == right.size()
            && std::equal(left.begin(), left.end(), right.begin());
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
            case QuicFrameType::Stream: {
                if ((type.Value().value & 0x02) == 0) return false;
                const auto parsed = ParseQuicStreamFrame(
                    bytes.data() + offset, bytes.size() - offset);
                if (!parsed.IsOk() || !parsed.Value().hasLength) return false;
                consumedBytes = parsed.Value().consumedBytes;
                break;
            }
            case QuicFrameType::ResetStream: {
                const auto parsed = ParseQuicResetStreamFrame(
                    bytes.data() + offset, bytes.size() - offset);
                if (!parsed.IsOk()) return false;
                consumedBytes = parsed.Value().consumedBytes;
                break;
            }
            case QuicFrameType::StopSending: {
                const auto parsed = ParseQuicStopSendingFrame(
                    bytes.data() + offset, bytes.size() - offset);
                if (!parsed.IsOk()) return false;
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
    const std::array<std::uint8_t, 2> firstData{ 0xA0, 0xA1 };
    const std::array<std::uint8_t, 3> finData{ 0xB0, 0xB1, 0xB2 };
    const std::array<std::uint8_t, 1> terminalData{ 0xC0 };

    const QuicStreamFrameBuildOptions firstStream{
        3, 0, false, true, false, firstData };
    const QuicStreamFrameBuildOptions finStream{
        3, 7, true, true, true, finData };
    const QuicResetStreamFrame reset{ 3, 9, 10, 0 };
    const QuicStopSendingFrame stop{ 3, 11, 0 };

    std::vector<std::uint8_t> sequence;
    Require(Append(sequence, BuildQuicStreamFrame(firstStream)),
        "length-bearing STREAM should build");
    Require(Append(sequence, BuildQuicStreamFrame(finStream)),
        "offset/FIN STREAM should build");
    Require(Append(sequence, BuildQuicResetStreamFrame(reset)),
        "RESET_STREAM should build");
    Require(Append(sequence, BuildQuicStopSendingFrame(stop)),
        "STOP_SENDING should build");

    std::vector<FrameObservation> observations;
    Require(DispatchSequence(sequence, observations)
        && observations.size() == 4,
        "mixed STREAM/control sequence should dispatch completely");
    const std::array<QuicFrameType, 4> expectedKinds{
        QuicFrameType::Stream, QuicFrameType::Stream,
        QuicFrameType::ResetStream, QuicFrameType::StopSending };
    std::size_t expectedOffset = 0;
    for (std::size_t index = 0; index < observations.size(); ++index) {
        Require(observations[index].kind == expectedKinds[index]
            && observations[index].offset == expectedOffset
            && observations[index].consumedBytes > 0,
            "stream/control coordinates should be monotonic");
        expectedOffset += observations[index].consumedBytes;
    }
    Require(expectedOffset == sequence.size(),
        "stream/control dispatch must consume the complete sequence");

    const auto parsedFirst = ParseQuicStreamFrame(
        sequence.data(), sequence.size());
    Require(parsedFirst.IsOk() && !parsedFirst.Value().hasOffset
        && parsedFirst.Value().hasLength && !parsedFirst.Value().fin
        && parsedFirst.Value().streamId == 3
        && SameBytes(parsedFirst.Value().data, firstData),
        "first STREAM optional fields should survive dispatch composition");
    const auto parsedFin = ParseQuicStreamFrame(
        sequence.data() + observations[1].offset,
        sequence.size() - observations[1].offset);
    Require(parsedFin.IsOk() && parsedFin.Value().hasOffset
        && parsedFin.Value().hasLength && parsedFin.Value().fin
        && parsedFin.Value().offset == 7
        && SameBytes(parsedFin.Value().data, finData),
        "FIN STREAM coordinates should survive dispatch composition");
    const auto parsedReset = ParseQuicResetStreamFrame(
        sequence.data() + observations[2].offset,
        sequence.size() - observations[2].offset);
    Require(parsedReset.IsOk() && parsedReset.Value().streamId == 3
        && parsedReset.Value().applicationErrorCode == 9
        && parsedReset.Value().finalSize == 10,
        "RESET_STREAM coordinates should survive dispatch composition");
    const auto parsedStop = ParseQuicStopSendingFrame(
        sequence.data() + observations[3].offset,
        sequence.size() - observations[3].offset);
    Require(parsedStop.IsOk() && parsedStop.Value().streamId == 3
        && parsedStop.Value().applicationErrorCode == 11,
        "STOP_SENDING coordinates should survive dispatch composition");

    auto streamWithTrailing = BuildQuicStreamFrame(firstStream).Value();
    streamWithTrailing.push_back(0xA5);
    const auto parsedTrailing = ParseQuicStreamFrame(
        streamWithTrailing.data(), streamWithTrailing.size());
    Require(parsedTrailing.IsOk()
        && parsedTrailing.Value().consumedBytes + 1 == streamWithTrailing.size(),
        "length-bearing STREAM parser must preserve trailing boundary");

    const auto lengthless = BuildQuicStreamFrame({
        3, 0, true, false, false, terminalData });
    Require(lengthless.IsOk(), "lengthless STREAM should build for terminal test");
    const auto parsedLengthless = ParseQuicStreamFrame(
        lengthless.Value().data(), lengthless.Value().size());
    Require(parsedLengthless.IsOk() && !parsedLengthless.Value().hasLength
        && parsedLengthless.Value().consumedBytes == lengthless.Value().size(),
        "lengthless STREAM parser should consume the enclosing tail");
    Require(!DispatchSequence(lengthless.Value(), observations)
        && observations.empty(),
        "lengthless STREAM must not be treated as an intermediate sequence frame");

    const auto oldObservations = observations;
    auto unknown = sequence;
    const auto extension = EncodeQuicVarInt(0x2a);
    Require(extension.IsOk(), "unknown extension type should encode");
    unknown.insert(unknown.begin() + static_cast<std::ptrdiff_t>(
        observations.empty() ? 0 : observations[0].offset),
        extension.Value().storage.begin(),
        extension.Value().storage.begin() + extension.Value().size);
    Require(!DispatchSequence(unknown, observations) && observations.empty(),
        "unknown stream extension must reject atomically");
    observations = oldObservations;

    auto truncated = sequence;
    truncated.pop_back();
    Require(!DispatchSequence(truncated, observations) && observations.empty(),
        "truncated STOP_SENDING must reject atomically");
    observations = oldObservations;

    const std::array<std::uint8_t, 1> malformedType{ 0x40 };
    Require(!DispatchSequence(malformedType, observations) && observations.empty(),
        "truncated frame-type varint must reject atomically");
    observations = oldObservations;

    const auto wrongKind = BuildQuicStopSendingFrame(stop);
    Require(wrongKind.IsOk()
        && !ParseQuicResetStreamFrame(
            wrongKind.Value().data(), wrongKind.Value().size()).IsOk(),
        "wrong stream-control parser kind must be rejected");

    std::cout << "passed=true"
              << " sequence_roundtrip=true"
              << " stream_variants=true"
              << " fin_coordinates=true"
              << " control_coordinates=true"
              << " trailing_boundary=true"
              << " lengthless_terminal=true"
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
