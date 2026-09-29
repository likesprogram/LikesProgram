#include <LikesProgram/Net/Connection.hpp>
#include "net/PollerAccess.hpp"
#include <utility>

namespace LikesProgram {
    namespace Net {
        namespace Internal {
            void PollerAccess::CompleteRead(Connection& connection, BufferLease&& lease) {
                connection.HandlePollerRead(std::move(lease));
            }

            void PollerAccess::CompleteRead(Connection& connection, Buffer&& input) {
                connection.HandlePollerRead(std::move(input));
            }

            std::size_t PollerAccess::MaxDatagramBytes(const Connection& connection) noexcept {
                return connection.GetMaxDatagramBytesForPoller();
            }

            void PollerAccess::CompleteDatagram(
                Connection& connection,
                Buffer& input,
                const Address& peer,
                std::size_t originalBytes,
                bool truncated) {
                connection.HandlePollerDatagram(input, peer, originalBytes, truncated);
            }

            void PollerAccess::DatagramWriteCompleted(
                Connection& connection,
                const Address& peer,
                std::size_t queueCost) noexcept {
                connection.HandlePollerDatagramWriteCompleted(peer, queueCost);
            }

            void PollerAccess::PeerClosed(Connection& connection) {
                connection.HandlePollerPeerClosed();
            }

            bool PollerAccess::ReadEnabled(const Connection& connection) noexcept {
                return connection.PollerReadEnabled();
            }

            bool PollerAccess::WriteGrowth(Connection& connection, std::size_t pendingBytes) {
                return connection.HandlePollerWriteGrowth(pendingBytes);
            }

            void PollerAccess::WriteDrain(Connection& connection, std::size_t pendingBytes) {
                connection.HandlePollerWriteDrain(pendingBytes);
            }

            void PollerAccess::WriteComplete(Connection& connection) {
                connection.HandlePollerWriteComplete();
            }

            void PollerAccess::Error(Connection& connection, int error) {
                connection.HandlePollerError(error);
            }

            void PollerAccess::Detach(Connection& connection) noexcept {
                connection.HandlePollerDetached();
            }
        }
    }
}
