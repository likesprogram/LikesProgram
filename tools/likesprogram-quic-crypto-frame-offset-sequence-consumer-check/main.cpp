#include <LikesProgram/Quic/QuicCryptoFrame.hpp>
#include <LikesProgram/Quic/QuicFrameType.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
    using namespace LikesProgram::Quic;

    struct FrameObservation {
        std::uint64_t offset = 0;
        std::size_t length = 0;
        std::size_t packetOffset = 0;
        std::size_t consumedBytes = 0;
        bool duplicate = false;

        bool operator==(const FrameObservation&) const = default;
    };

    struct CryptoRange {
        std::uint64_t offset = 0;
        std::vector<std::uint8_t> data;
    };

    struct CryptoObservationState {
        std::uint64_t nextOffset = 0;
        std::vector<CryptoRange> ranges;
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

    bool ObserveCryptoFrame(const QuicCryptoFrame& frame,
        CryptoObservationState& state,
        bool& duplicate) {
        duplicate = false;
        if (frame.data.empty()
            || frame.offset > std::numeric_limits<std::uint64_t>::max()
                - frame.data.size()) {
            return false;
        }

        for (const auto& range : state.ranges) {
            if (range.offset == frame.offset) {
                if (!SameBytes(range.data, frame.data)) return false;
                duplicate = true;
                return true;
            }
        }

        if (!state.ranges.empty() && frame.offset != state.nextOffset) {
            return false;
        }

        CryptoRange range;
        range.offset = frame.offset;
        range.data.assign(frame.data.begin(), frame.data.end());
        state.ranges.push_back(std::move(range));
        state.nextOffset = frame.offset + frame.data.size();
        return true;
    }

    bool DispatchCryptoSequence(std::span<const std::uint8_t> bytes,
        CryptoObservationState& state,
        std::vector<FrameObservation>& observations) {
        CryptoObservationState pendingState = state;
        std::vector<FrameObservation> pendingObservations;
        std::size_t packetOffset = 0;
        while (packetOffset < bytes.size()) {
            const auto type = ParseQuicFrameType(
                bytes.data() + packetOffset, bytes.size() - packetOffset);
            if (!type.IsOk() || type.Value().kind != QuicFrameType::Crypto) {
                return false;
            }

            const auto parsed = ParseQuicCryptoFrame(
                bytes.data() + packetOffset, bytes.size() - packetOffset);
            if (!parsed.IsOk()
                || parsed.Value().consumedBytes < type.Value().encodedBytes
                || parsed.Value().consumedBytes > bytes.size() - packetOffset) {
                return false;
            }

            bool duplicate = false;
            if (!ObserveCryptoFrame(parsed.Value(), pendingState, duplicate)) {
                return false;
            }
            pendingObservations.push_back({
                parsed.Value().offset,
                parsed.Value().data.size(),
                packetOffset,
                parsed.Value().consumedBytes,
                duplicate });
            packetOffset += parsed.Value().consumedBytes;
        }

        state = std::move(pendingState);
        observations = std::move(pendingObservations);
        return true;
    }
}

int Run() {
    using namespace LikesProgram::Quic;
    const std::array<std::uint8_t, 3> firstData{ 0x10, 0x11, 0x12 };
    const std::array<std::uint8_t, 2> secondData{ 0x20, 0x21 };
    const std::array<std::uint8_t, 1> thirdData{ 0x30 };

    const QuicCryptoFrameBuildOptions first{ 4, firstData };
    const QuicCryptoFrameBuildOptions second{ 7, secondData };
    const QuicCryptoFrameBuildOptions third{ 9, thirdData };

    const auto firstBytes = BuildQuicCryptoFrame(first);
    const auto secondBytes = BuildQuicCryptoFrame(second);
    const auto thirdBytes = BuildQuicCryptoFrame(third);
    Require(firstBytes.IsOk() && secondBytes.IsOk() && thirdBytes.IsOk(),
        "contiguous CRYPTO frames should build");

    std::vector<std::uint8_t> sequence;
    Require(Append(sequence, firstBytes), "first CRYPTO frame should append");
    Require(Append(sequence, firstBytes), "duplicate CRYPTO frame should append");
    Require(Append(sequence, secondBytes), "second CRYPTO frame should append");
    Require(Append(sequence, thirdBytes), "third CRYPTO frame should append");

    CryptoObservationState state;
    std::vector<FrameObservation> observations;
    Require(DispatchCryptoSequence(sequence, state, observations)
        && observations.size() == 4
        && state.ranges.size() == 3
        && state.nextOffset == 10,
        "duplicate and contiguous CRYPTO sequence should commit");
    Require(observations[0].offset == 4 && observations[0].length == 3
        && observations[0].packetOffset == 0 && !observations[0].duplicate,
        "initial CRYPTO coordinates should be recorded");
    Require(observations[1].offset == 4 && observations[1].duplicate,
        "identical duplicate CRYPTO frame should be accepted deterministically");
    Require(observations[2].offset == 7 && observations[2].length == 2
        && observations[3].offset == 9 && observations[3].length == 1,
        "contiguous CRYPTO offsets should advance by payload length");
    Require(observations[3].packetOffset + observations[3].consumedBytes
        == sequence.size(),
        "CRYPTO sequence should preserve complete packet boundary");

    const auto firstParsed = ParseQuicCryptoFrame(
        firstBytes.Value().data(), firstBytes.Value().size());
    Require(firstParsed.IsOk() && firstParsed.Value().offset == 4
        && SameBytes(firstParsed.Value().data, firstData),
        "CRYPTO parser should preserve offset and payload");

    const auto baselineState = state;
    const auto baselineObservations = observations;

    auto gap = secondBytes.Value();
    const auto gapFrame = BuildQuicCryptoFrame({ 8, secondData });
    Require(gapFrame.IsOk(), "gap CRYPTO frame should build");
    gap = gapFrame.Value();
    CryptoObservationState rejectedState = baselineState;
    std::vector<FrameObservation> rejectedObservations = baselineObservations;
    Require(!DispatchCryptoSequence(gap, rejectedState, rejectedObservations)
        && rejectedState.ranges.size() == baselineState.ranges.size()
        && rejectedState.nextOffset == baselineState.nextOffset
        && rejectedObservations == baselineObservations,
        "gap input must reject without mutating state");

    auto overlapFrame = BuildQuicCryptoFrame({ 6, secondData });
    Require(overlapFrame.IsOk(), "overlap CRYPTO frame should build");
    rejectedState = baselineState;
    rejectedObservations = baselineObservations;
    Require(!DispatchCryptoSequence(overlapFrame.Value(),
        rejectedState, rejectedObservations)
        && rejectedState.ranges.size() == baselineState.ranges.size()
        && rejectedState.nextOffset == baselineState.nextOffset
        && rejectedObservations == baselineObservations,
        "overlap input must reject without mutating state");

    auto conflictingDuplicate = BuildQuicCryptoFrame({ 4,
        std::array<std::uint8_t, 3>{ 0x99, 0x98, 0x97 } });
    Require(conflictingDuplicate.IsOk(),
        "conflicting duplicate CRYPTO frame should build");
    rejectedState = baselineState;
    rejectedObservations = baselineObservations;
    Require(!DispatchCryptoSequence(conflictingDuplicate.Value(),
        rejectedState, rejectedObservations)
        && rejectedState.ranges.size() == baselineState.ranges.size()
        && rejectedState.nextOffset == baselineState.nextOffset
        && rejectedObservations == baselineObservations,
        "conflicting duplicate must reject without mutating state");

    auto truncated = sequence;
    truncated.pop_back();
    rejectedState = baselineState;
    rejectedObservations = baselineObservations;
    Require(!DispatchCryptoSequence(truncated, rejectedState,
        rejectedObservations)
        && rejectedState.ranges.size() == baselineState.ranges.size()
        && rejectedState.nextOffset == baselineState.nextOffset
        && rejectedObservations == baselineObservations,
        "truncated CRYPTO sequence must reject atomically");

    auto unknown = firstBytes.Value();
    const auto extension = EncodeQuicVarInt(0x2a);
    Require(extension.IsOk(), "unknown extension type should encode");
    unknown.insert(unknown.begin(), extension.Value().storage.begin(),
        extension.Value().storage.begin() + extension.Value().size);
    rejectedState = baselineState;
    rejectedObservations = baselineObservations;
    Require(!DispatchCryptoSequence(unknown, rejectedState,
        rejectedObservations)
        && rejectedState.ranges.size() == baselineState.ranges.size()
        && rejectedState.nextOffset == baselineState.nextOffset
        && rejectedObservations == baselineObservations,
        "unknown frame type must reject atomically");

    const std::array<std::uint8_t, 1> malformedType{ 0x40 };
    rejectedState = baselineState;
    rejectedObservations = baselineObservations;
    Require(!DispatchCryptoSequence(malformedType, rejectedState,
        rejectedObservations)
        && rejectedState.ranges.size() == baselineState.ranges.size()
        && rejectedState.nextOffset == baselineState.nextOffset
        && rejectedObservations == baselineObservations,
        "malformed frame type must reject atomically");

    std::cout << "passed=true"
              << " sequence_roundtrip=true"
              << " offsets=true"
              << " duplicate=true"
              << " gap_rejected=true"
              << " overlap_rejected=true"
              << " conflicting_duplicate_rejected=true"
              << " truncated_rejected=true"
              << " unknown_rejected=true"
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
