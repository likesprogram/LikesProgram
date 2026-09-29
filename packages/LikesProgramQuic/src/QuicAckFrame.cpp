#include <LikesProgram/Quic/QuicAckFrame.hpp>

#include <LikesProgram/Quic/QuicVarInt.hpp>

#include <algorithm>
#include <limits>

namespace {
    using LikesProgram::Quic::QuicAckFrame;
    using LikesProgram::Quic::QuicAckRange;
    using LikesProgram::Quic::QuicVarIntValue;

    LikesProgram::Result<QuicVarIntValue> ReadVarInt(
        const std::uint8_t* data,
        std::size_t size,
        std::size_t& offset) {
        if (offset > size) {
            return LikesProgram::Status::InvalidArgument(u"QUIC ACK offset is invalid");
        }
        const auto value = LikesProgram::Quic::ParseQuicVarInt(
            data + offset, size - offset);
        if (!value.IsOk()) return value.GetStatus();
        offset += value.Value().encodedBytes;
        return value;
    }

    bool IsFrameType(const QuicVarIntValue& value, bool& ecn) noexcept {
        if (value.value == 0x02) {
            ecn = false;
            return true;
        }
        if (value.value == 0x03) {
            ecn = true;
            return true;
        }
        return false;
    }

    LikesProgram::Result<std::vector<std::uint8_t>> EncodeVarInt(
        std::uint64_t value) {
        const auto encoded = LikesProgram::Quic::EncodeQuicVarInt(value);
        if (!encoded.IsOk()) return encoded.GetStatus();
        return std::vector<std::uint8_t>(
            encoded.Value().storage.begin(),
            encoded.Value().storage.begin() + encoded.Value().size);
    }

    bool IsValidRange(const QuicAckRange& range) noexcept {
        return range.smallest <= range.largest;
    }
}

namespace LikesProgram {
    namespace Quic {
        Result<QuicAckFrame> ParseQuicAckFrame(
            const std::uint8_t* data,
            std::size_t size) {
            if (data == nullptr || size == 0) {
                return Status::InvalidArgument(u"QUIC ACK frame is empty");
            }

            std::size_t offset = 0;
            const auto type = ReadVarInt(data, size, offset);
            if (!type.IsOk()) return type.GetStatus();
            bool ecn = false;
            if (!IsFrameType(type.Value(), ecn)) {
                return Status::InvalidArgument(u"QUIC frame is not ACK or ACK_ECN");
            }

            const auto largest = ReadVarInt(data, size, offset);
            if (!largest.IsOk()) return largest.GetStatus();
            const auto delay = ReadVarInt(data, size, offset);
            if (!delay.IsOk()) return delay.GetStatus();
            const auto rangeCount = ReadVarInt(data, size, offset);
            if (!rangeCount.IsOk()) return rangeCount.GetStatus();
            const auto firstRange = ReadVarInt(data, size, offset);
            if (!firstRange.IsOk()) return firstRange.GetStatus();
            if (firstRange.Value().value > largest.Value().value) {
                return Status::InvalidArgument(u"QUIC ACK first range exceeds largest");
            }

            QuicAckFrame frame;
            frame.ecn = ecn;
            frame.largestAcknowledged = largest.Value().value;
            frame.ackDelay = delay.Value().value;
            const auto availablePairs = static_cast<std::uint64_t>(
                (size - offset) / 2 + 1);
            frame.ranges.reserve(static_cast<std::size_t>(std::min(
                rangeCount.Value().value + 1, availablePairs)));
            frame.ranges.push_back({
                largest.Value().value - firstRange.Value().value,
                largest.Value().value });

            std::uint64_t previousSmallest = frame.ranges.front().smallest;
            for (std::uint64_t index = 0; index < rangeCount.Value().value; ++index) {
                const auto gap = ReadVarInt(data, size, offset);
                if (!gap.IsOk()) return gap.GetStatus();
                const auto range = ReadVarInt(data, size, offset);
                if (!range.IsOk()) return range.GetStatus();
                if (gap.Value().value > kQuicVarIntMaximum - 2
                    || previousSmallest < gap.Value().value + 2) {
                    return Status::InvalidArgument(u"QUIC ACK gap underflows packet number");
                }
                const std::uint64_t nextLargest =
                    previousSmallest - gap.Value().value - 2;
                if (range.Value().value > nextLargest) {
                    return Status::InvalidArgument(u"QUIC ACK range underflows packet number");
                }
                const std::uint64_t nextSmallest =
                    nextLargest - range.Value().value;
                frame.ranges.push_back({ nextSmallest, nextLargest });
                previousSmallest = nextSmallest;
            }

            if (frame.ecn) {
                const auto ect0 = ReadVarInt(data, size, offset);
                if (!ect0.IsOk()) return ect0.GetStatus();
                const auto ect1 = ReadVarInt(data, size, offset);
                if (!ect1.IsOk()) return ect1.GetStatus();
                const auto ce = ReadVarInt(data, size, offset);
                if (!ce.IsOk()) return ce.GetStatus();
                frame.ect0Count = ect0.Value().value;
                frame.ect1Count = ect1.Value().value;
                frame.ecnCeCount = ce.Value().value;
            }
            frame.consumedBytes = offset;
            return frame;
        }

        Result<std::vector<std::uint8_t>> BuildQuicAckFrame(
            const QuicAckFrame& frame) {
            if (frame.ranges.empty() || frame.ranges.size() - 1 > kQuicVarIntMaximum
                || frame.largestAcknowledged > kQuicVarIntMaximum
                || frame.ackDelay > kQuicVarIntMaximum
                || frame.ect0Count > kQuicVarIntMaximum
                || frame.ect1Count > kQuicVarIntMaximum
                || frame.ecnCeCount > kQuicVarIntMaximum) {
                return Status::InvalidArgument(u"QUIC ACK frame values are invalid");
            }
            if (!IsValidRange(frame.ranges.front())
                || frame.ranges.front().largest != frame.largestAcknowledged
                || frame.ranges.front().largest - frame.ranges.front().smallest
                    > kQuicVarIntMaximum) {
                return Status::InvalidArgument(u"QUIC ACK first range is invalid");
            }

            std::size_t reserve = 1 + 8 * 4;
            for (std::size_t index = 0; index < frame.ranges.size(); ++index) {
                if (!IsValidRange(frame.ranges[index])) {
                    return Status::InvalidArgument(u"QUIC ACK range is invalid");
                }
                reserve += 16;
                if (index == 0) continue;
                const auto& previous = frame.ranges[index - 1];
                const auto& current = frame.ranges[index];
                if (previous.smallest < current.largest
                    || previous.smallest - current.largest < 2) {
                    return Status::InvalidArgument(u"QUIC ACK ranges overlap or touch");
                }
            }

            std::vector<std::uint8_t> output;
            output.reserve(reserve);
            const auto append = [&output](std::uint64_t value) -> bool {
                const auto encoded = EncodeVarInt(value);
                if (!encoded.IsOk()) return false;
                output.insert(output.end(), encoded.Value().begin(), encoded.Value().end());
                return true;
            };
            if (!append(frame.ecn ? 0x03 : 0x02)
                || !append(frame.largestAcknowledged)
                || !append(frame.ackDelay)
                || !append(frame.ranges.size() - 1)
                || !append(frame.ranges.front().largest - frame.ranges.front().smallest)) {
                return Status::InvalidArgument(u"QUIC ACK value cannot be encoded");
            }
            for (std::size_t index = 1; index < frame.ranges.size(); ++index) {
                const auto& previous = frame.ranges[index - 1];
                const auto& current = frame.ranges[index];
                if (!append(previous.smallest - current.largest - 2)
                    || !append(current.largest - current.smallest)) {
                    return Status::InvalidArgument(u"QUIC ACK range cannot be encoded");
                }
            }
            if (frame.ecn
                && (!append(frame.ect0Count)
                    || !append(frame.ect1Count)
                    || !append(frame.ecnCeCount))) {
                return Status::InvalidArgument(u"QUIC ACK ECN counts cannot be encoded");
            }
            return output;
        }
    }
}
