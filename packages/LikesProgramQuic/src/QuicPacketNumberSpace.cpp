#include <LikesProgram/Quic/QuicPacketNumberSpace.hpp>

namespace LikesProgram {
    namespace Quic {
        Result<std::uint64_t> QuicPacketNumberSpace::Decode(
            std::uint64_t truncatedPacketNumber,
            std::size_t encodedBytes) {
            const auto decoded = DecodeQuicPacketNumber(
                m_hasLargestReceived ? m_largestReceived : 0,
                truncatedPacketNumber,
                encodedBytes);
            if (!decoded.IsOk()) return decoded.GetStatus();
            if (!m_hasLargestReceived || decoded.Value() > m_largestReceived) {
                m_hasLargestReceived = true;
                m_largestReceived = decoded.Value();
            }
            return decoded;
        }

        Result<void> QuicPacketNumberSpace::Observe(
            std::uint64_t fullPacketNumber) noexcept {
            constexpr std::uint64_t maximum =
                (std::uint64_t{ 1 } << 62) - 1;
            if (fullPacketNumber > maximum) {
                return Status::InvalidArgument(
                    u"QUIC packet number observation is outside the 62-bit space");
            }
            if (!m_hasLargestReceived || fullPacketNumber > m_largestReceived) {
                m_hasLargestReceived = true;
                m_largestReceived = fullPacketNumber;
            }
            return {};
        }

        QuicPacketNumberSpaceSnapshot
            QuicPacketNumberSpace::Snapshot() const noexcept {
            return { m_hasLargestReceived, m_largestReceived };
        }

        void QuicPacketNumberSpace::Reset() noexcept {
            m_hasLargestReceived = false;
            m_largestReceived = 0;
        }
    }
}
