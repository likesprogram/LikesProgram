#include <LikesProgram/Quic/QuicPacketNumber.hpp>

#include <bit>

namespace {
    constexpr std::uint64_t kPacketNumberMaximum =
        (std::uint64_t{ 1 } << 62) - 1;

    std::size_t RequiredBits(std::uint64_t outstanding) noexcept {
        if (outstanding == 0) return 0;
        return std::bit_width(outstanding - 1) + 1;
    }

    bool IsPacketNumberWidth(std::size_t width) noexcept {
        return width >= 1 && width <= 4;
    }
}

namespace LikesProgram {
    namespace Quic {
        Result<QuicPacketNumberEncoding> EncodeQuicPacketNumber(
            std::uint64_t fullPacketNumber,
            std::optional<std::uint64_t> largestAcknowledged) {
            if (fullPacketNumber > kPacketNumberMaximum
                || (largestAcknowledged.has_value()
                    && (*largestAcknowledged > fullPacketNumber
                        || *largestAcknowledged > kPacketNumberMaximum))) {
                return Status::InvalidArgument(u"QUIC packet number is invalid");
            }

            const std::uint64_t outstanding = largestAcknowledged.has_value()
                ? fullPacketNumber - *largestAcknowledged
                : fullPacketNumber + 1;
            const std::size_t requiredBits = RequiredBits(outstanding);
            const std::size_t width = (requiredBits + 7) / 8;
            if (!IsPacketNumberWidth(width)) {
                return Status::InvalidArgument(
                    u"QUIC packet number requires more than four bytes");
            }

            QuicPacketNumberEncoding output;
            output.size = width;
            for (std::size_t index = width; index > 0; --index) {
                output.storage[index - 1] =
                    static_cast<std::uint8_t>(fullPacketNumber & 0xFF);
                fullPacketNumber >>= 8;
            }
            return output;
        }

        Result<std::uint64_t> DecodeQuicPacketNumber(
            std::uint64_t largestReceivedPacketNumber,
            std::uint64_t truncatedPacketNumber,
            std::size_t encodedBytes) {
            if (!IsPacketNumberWidth(encodedBytes)
                || largestReceivedPacketNumber >= kPacketNumberMaximum) {
                return Status::InvalidArgument(u"QUIC packet number decode input is invalid");
            }

            const std::size_t packetNumberBits = encodedBytes * 8;
            const std::uint64_t packetNumberWindow =
                std::uint64_t{ 1 } << packetNumberBits;
            const std::uint64_t packetNumberHalfWindow = packetNumberWindow / 2;
            const std::uint64_t packetNumberMask = packetNumberWindow - 1;
            if (truncatedPacketNumber > packetNumberMask) {
                return Status::InvalidArgument(
                    u"QUIC truncated packet number exceeds encoded width");
            }

            const std::uint64_t expectedPacketNumber =
                largestReceivedPacketNumber + 1;
            const std::uint64_t candidatePacketNumber =
                (expectedPacketNumber & ~packetNumberMask) | truncatedPacketNumber;

            if (expectedPacketNumber >= packetNumberHalfWindow
                && candidatePacketNumber
                    <= expectedPacketNumber - packetNumberHalfWindow
                && candidatePacketNumber < kPacketNumberMaximum - packetNumberWindow) {
                return candidatePacketNumber + packetNumberWindow;
            }
            if (candidatePacketNumber > expectedPacketNumber
                && candidatePacketNumber - expectedPacketNumber
                    > packetNumberHalfWindow
                && candidatePacketNumber >= packetNumberWindow) {
                return candidatePacketNumber - packetNumberWindow;
            }
            return candidatePacketNumber;
        }
    }
}
