#include <LikesProgram/Quic/QuicAckApplication.hpp>

#include <limits>

namespace {
    constexpr std::uint64_t kQuicPacketNumberMaximum =
        (std::uint64_t{ 1 } << 62) - 1;
}

namespace LikesProgram {
    namespace Quic {
        Result<QuicAckApplicationResult> ApplyQuicAckFrame(
            QuicRetransmissionQueue& queue,
            const QuicAckFrame& frame,
            QuicAckApplicationLimits limits) {
            if (frame.ranges.empty() || limits.maxPacketNumbers == 0
                || frame.largestAcknowledged > kQuicPacketNumberMaximum
                || frame.ranges.front().largest != frame.largestAcknowledged) {
                return Status::InvalidArgument(u"QUIC ACK application input is invalid");
            }

            std::uint64_t covered = 0;
            for (std::size_t index = 0; index < frame.ranges.size(); ++index) {
                const auto& range = frame.ranges[index];
                if (range.smallest > range.largest
                    || range.largest > kQuicPacketNumberMaximum) {
                    return Status::InvalidArgument(u"QUIC ACK range is invalid");
                }
                if (index > 0) {
                    const auto& previous = frame.ranges[index - 1];
                    if (previous.smallest == 0
                        || range.largest >= previous.smallest - 1) {
                        return Status::InvalidArgument(
                            u"QUIC ACK ranges overlap or are out of order");
                    }
                }

                const std::uint64_t span = range.largest - range.smallest + 1;
                if (span > limits.maxPacketNumbers - covered) {
                    return Status(StatusCode::ResourceExhausted,
                        u"QUIC ACK range coverage exceeds the configured bound");
                }
                covered += span;
            }

            QuicAckApplicationResult result;
            for (const auto& range : frame.ranges) {
                for (std::uint64_t packetNumber = range.smallest;; ++packetNumber) {
                    const auto acknowledged = queue.Acknowledge(packetNumber);
                    if (acknowledged.IsOk()) {
                        ++result.acknowledgedPackets;
                    } else if (acknowledged.GetStatus().Code() == StatusCode::NotFound) {
                        ++result.untrackedPackets;
                    } else {
                        return acknowledged.GetStatus();
                    }
                    if (packetNumber == range.largest) break;
                }
            }
            return result;
        }
    }
}
