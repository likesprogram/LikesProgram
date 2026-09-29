#include <LikesProgram/Quic/QuicShortHeader.hpp>

namespace {
    constexpr std::size_t kQuicV1ConnectionIdMaximum = 20;
}

namespace LikesProgram {
    namespace Quic {
        Result<QuicShortHeaderView> ParseQuicShortHeader(
            const std::uint8_t* data,
            std::size_t size,
            std::size_t destinationConnectionIdLength) {
            if (data == nullptr || size < 1
                || destinationConnectionIdLength > kQuicV1ConnectionIdMaximum) {
                return Status::InvalidArgument(u"QUIC short header is incomplete");
            }
            const std::uint8_t first = data[0];
            if ((first & 0x80) != 0 || (first & 0x40) == 0) {
                return Status::InvalidArgument(
                    u"QUIC packet is not a fixed-bit short header");
            }
            if ((first & 0x18) != 0) {
                return Status::InvalidArgument(
                    u"QUIC short header reserved bits are set");
            }
            const std::size_t packetNumberLength =
                static_cast<std::size_t>((first & 0x03) + 1);
            const std::size_t headerBytes = 1 + destinationConnectionIdLength
                + packetNumberLength;
            if (headerBytes > size) {
                return Status::InvalidArgument(
                    u"QUIC short header packet number is truncated");
            }
            QuicShortHeaderView view;
            view.spinBit = (first & 0x20) != 0;
            view.keyPhase = (first & 0x04) != 0;
            view.packetNumberLength = packetNumberLength;
            view.destinationConnectionId = {
                data + 1, destinationConnectionIdLength };
            view.packetNumber = {
                data + 1 + destinationConnectionIdLength, packetNumberLength };
            view.payload = {
                data + headerBytes, size - headerBytes };
            view.consumedBytes = size;
            return view;
        }

        Result<std::vector<std::uint8_t>> BuildQuicShortHeaderPacket(
            const QuicShortHeaderBuildOptions& options) {
            if (options.destinationConnectionId.size() > kQuicV1ConnectionIdMaximum
                || options.packetNumber.empty() || options.packetNumber.size() > 4) {
                return Status::InvalidArgument(u"QUIC short header options are invalid");
            }
            std::vector<std::uint8_t> output;
            output.reserve(1 + options.destinationConnectionId.size()
                + options.packetNumber.size() + options.payload.size());
            std::uint8_t first = 0x40;
            if (options.spinBit) first = static_cast<std::uint8_t>(first | 0x20);
            if (options.keyPhase) first = static_cast<std::uint8_t>(first | 0x04);
            first = static_cast<std::uint8_t>(first
                | (options.packetNumber.size() - 1));
            output.push_back(first);
            output.insert(output.end(), options.destinationConnectionId.begin(),
                options.destinationConnectionId.end());
            output.insert(output.end(), options.packetNumber.begin(),
                options.packetNumber.end());
            output.insert(output.end(), options.payload.begin(), options.payload.end());
            return output;
        }
    }
}
