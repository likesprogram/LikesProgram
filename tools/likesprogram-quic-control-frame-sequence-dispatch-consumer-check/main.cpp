#include <LikesProgram/Quic/QuicAckFrame.hpp>
#include <LikesProgram/Quic/QuicConnectionCloseFrame.hpp>
#include <LikesProgram/Quic/QuicFlowControlFrame.hpp>
#include <LikesProgram/Quic/QuicFrameType.hpp>
#include <LikesProgram/Quic/QuicPathValidationFrame.hpp>

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
            case QuicFrameType::Ack: {
                const auto parsed = ParseQuicAckFrame(
                    bytes.data() + offset, bytes.size() - offset);
                if (!parsed.IsOk()) return false;
                consumedBytes = parsed.Value().consumedBytes;
                break;
            }
            case QuicFrameType::PathChallenge:
            case QuicFrameType::PathResponse: {
                const auto parsed = ParseQuicPathValidationFrame(
                    bytes.data() + offset, bytes.size() - offset);
                if (!parsed.IsOk()) return false;
                consumedBytes = parsed.Value().consumedBytes;
                break;
            }
            case QuicFrameType::MaxData:
            case QuicFrameType::MaxStreamData:
            case QuicFrameType::MaxStreams:
            case QuicFrameType::DataBlocked:
            case QuicFrameType::StreamDataBlocked:
            case QuicFrameType::StreamsBlocked: {
                const auto parsed = ParseQuicFlowControlFrame(
                    bytes.data() + offset, bytes.size() - offset);
                if (!parsed.IsOk()) return false;
                consumedBytes = parsed.Value().consumedBytes;
                break;
            }
            case QuicFrameType::ConnectionClose: {
                const auto parsed = ParseQuicConnectionCloseFrame(
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
    const std::array<std::uint8_t, 8> challengeData{
        1, 2, 3, 4, 5, 6, 7, 8 };
    const std::array<std::uint8_t, 8> responseData{
        9, 10, 11, 12, 13, 14, 15, 16 };
    const std::array<std::uint8_t, 3> transportReason{ 'b', 'y', 'e' };
    const std::array<std::uint8_t, 3> applicationReason{ 'a', 'p', 'p' };

    const QuicAckFrame ack{
        false, 42, 2, { { 40, 42 }, { 35, 37 } }, 0, 0, 0, 0 };
    const QuicAckFrame ackEcn{
        true, 50, 3, { { 49, 50 } }, 4, 5, 6, 0 };
    const QuicPathValidationFrame challenge{
        QuicPathValidationFrameKind::PathChallenge, challengeData, 0 };
    const QuicPathValidationFrame response{
        QuicPathValidationFrameKind::PathResponse, responseData, 0 };
    const QuicFlowControlFrame maxData{
        QuicFlowControlFrameKind::MaxData, 0, 100, 0 };
    const QuicFlowControlFrame maxStreamData{
        QuicFlowControlFrameKind::MaxStreamData, 4, 200, 0 };
    const QuicConnectionCloseFrame transportClose{
        false, 0x10, 0x1a, transportReason, 0 };
    const QuicConnectionCloseFrame applicationClose{
        true, 0x100, 0, applicationReason, 0 };

    std::vector<std::uint8_t> sequence;
    Require(Append(sequence, BuildQuicAckFrame(ack)), "ACK should build");
    Require(Append(sequence, BuildQuicAckFrame(ackEcn)), "ACK_ECN should build");
    Require(Append(sequence, BuildQuicPathValidationFrame(challenge)),
        "PATH_CHALLENGE should build");
    Require(Append(sequence, BuildQuicPathValidationFrame(response)),
        "PATH_RESPONSE should build");
    Require(Append(sequence, BuildQuicFlowControlFrame(maxData)),
        "MAX_DATA should build");
    Require(Append(sequence, BuildQuicFlowControlFrame(maxStreamData)),
        "MAX_STREAM_DATA should build");
    Require(Append(sequence, BuildQuicConnectionCloseFrame(transportClose)),
        "transport CONNECTION_CLOSE should build");
    Require(Append(sequence, BuildQuicConnectionCloseFrame(applicationClose)),
        "application CONNECTION_CLOSE should build");

    std::vector<FrameObservation> observations;
    Require(DispatchSequence(sequence, observations)
        && observations.size() == 8,
        "mixed control-frame sequence should dispatch completely");
    const std::array<QuicFrameType, 8> expectedKinds{
        QuicFrameType::Ack, QuicFrameType::Ack, QuicFrameType::PathChallenge,
        QuicFrameType::PathResponse, QuicFrameType::MaxData,
        QuicFrameType::MaxStreamData, QuicFrameType::ConnectionClose,
        QuicFrameType::ConnectionClose };
    std::size_t expectedOffset = 0;
    for (std::size_t index = 0; index < observations.size(); ++index) {
        Require(observations[index].kind == expectedKinds[index]
            && observations[index].offset == expectedOffset
            && observations[index].consumedBytes > 0,
            "control-frame coordinates should be monotonic");
        expectedOffset += observations[index].consumedBytes;
    }
    Require(expectedOffset == sequence.size(),
        "control-frame dispatch must consume the complete sequence");

    const auto parsedAck = ParseQuicAckFrame(
        sequence.data(), sequence.size());
    Require(parsedAck.IsOk() && !parsedAck.Value().ecn
        && parsedAck.Value().ranges.size() == 2,
        "ACK optional range fields should survive dispatch composition");
    const auto parsedEcn = ParseQuicAckFrame(
        sequence.data() + observations[1].offset,
        sequence.size() - observations[1].offset);
    Require(parsedEcn.IsOk() && parsedEcn.Value().ecn
        && parsedEcn.Value().ect0Count == 4
        && parsedEcn.Value().ect1Count == 5
        && parsedEcn.Value().ecnCeCount == 6,
        "ACK_ECN counters should survive dispatch composition");
    const auto parsedPath = ParseQuicPathValidationFrame(
        sequence.data() + observations[2].offset,
        sequence.size() - observations[2].offset);
    Require(parsedPath.IsOk()
        && parsedPath.Value().kind == QuicPathValidationFrameKind::PathChallenge
        && parsedPath.Value().data == challengeData,
        "PATH_CHALLENGE token should survive dispatch composition");
    const auto parsedStreamCredit = ParseQuicFlowControlFrame(
        sequence.data() + observations[5].offset,
        sequence.size() - observations[5].offset);
    Require(parsedStreamCredit.IsOk()
        && parsedStreamCredit.Value().streamId == 4
        && parsedStreamCredit.Value().limit == 200,
        "stream flow-control coordinates should survive dispatch composition");
    const auto parsedApplicationClose = ParseQuicConnectionCloseFrame(
        sequence.data() + observations[7].offset,
        sequence.size() - observations[7].offset);
    Require(parsedApplicationClose.IsOk()
        && parsedApplicationClose.Value().application
        && parsedApplicationClose.Value().errorCode == 0x100,
        "application close fields should survive dispatch composition");

    auto ackWithTrailing = BuildQuicAckFrame(ack).Value();
    ackWithTrailing.push_back(0xA5);
    const auto parsedTrailingAck = ParseQuicAckFrame(
        ackWithTrailing.data(), ackWithTrailing.size());
    Require(parsedTrailingAck.IsOk()
        && parsedTrailingAck.Value().consumedBytes + 1 == ackWithTrailing.size(),
        "ACK parser must preserve trailing boundary");

    const auto oldObservations = observations;
    auto unknown = sequence;
    const auto extension = EncodeQuicVarInt(0x2a);
    Require(extension.IsOk(), "unknown extension type should encode");
    unknown.insert(unknown.begin() + static_cast<std::ptrdiff_t>(
        observations[2].offset), extension.Value().storage.begin(),
        extension.Value().storage.begin() + extension.Value().size);
    Require(!DispatchSequence(unknown, observations) && observations.empty(),
        "unknown control-frame extension must reject atomically");
    observations = oldObservations;

    auto truncated = sequence;
    truncated.pop_back();
    Require(!DispatchSequence(truncated, observations) && observations.empty(),
        "truncated close reason must reject atomically");
    observations = oldObservations;

    const std::array<std::uint8_t, 1> malformedType{ 0x40 };
    Require(!DispatchSequence(malformedType, observations) && observations.empty(),
        "truncated frame-type varint must reject atomically");
    observations = oldObservations;

    const auto wrongKind = BuildQuicPathValidationFrame(response);
    Require(wrongKind.IsOk()
        && !ParseQuicAckFrame(wrongKind.Value().data(), wrongKind.Value().size()).IsOk(),
        "wrong parser kind must be rejected");

    std::cout << "passed=true"
              << " sequence_roundtrip=true"
              << " ack_ecn=true"
              << " path_variants=true"
              << " flow_coordinates=true"
              << " close_variants=true"
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
