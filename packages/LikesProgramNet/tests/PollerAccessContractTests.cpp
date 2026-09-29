#include "net/PollerAccess.hpp"

#include <LikesProgram/Net/Address.hpp>
#include <LikesProgram/Net/Buffer.hpp>
#include <LikesProgram/Net/BufferLease.hpp>
#include <LikesProgram/Net/Connection.hpp>
#include <concepts>

namespace {
    template<typename AccessType>
    concept CompletionConnectionBridge = requires(
        LikesProgram::Net::Connection& connection,
        LikesProgram::Net::Buffer& datagram,
        const LikesProgram::Net::Address& peer,
        LikesProgram::Net::BufferLease&& lease,
        std::size_t originalBytes,
        std::size_t pendingBytes,
        int error) {
        AccessType::CompleteRead(connection, static_cast<LikesProgram::Net::BufferLease&&>(lease));
        AccessType::CompleteDatagram(connection, datagram, peer, originalBytes, true);
        AccessType::DatagramWriteCompleted(connection, peer, pendingBytes);
        AccessType::PeerClosed(connection);
        { AccessType::MaxDatagramBytes(connection) } -> std::same_as<std::size_t>;
        { AccessType::ReadEnabled(connection) } -> std::same_as<bool>;
        { AccessType::WriteGrowth(connection, pendingBytes) } -> std::same_as<bool>;
        AccessType::WriteDrain(connection, pendingBytes);
        AccessType::WriteComplete(connection);
        AccessType::Error(connection, error);
        AccessType::Detach(connection);
    };

    static_assert(
        CompletionConnectionBridge<LikesProgram::Net::Internal::PollerAccess>,
        "Every completion backend should reuse the common private Connection bridge");

    template<typename ConnectionType>
    concept DatagramConnectionContract = requires(
        ConnectionType& connection,
        const LikesProgram::Net::Address& peer,
        LikesProgram::Net::Buffer&& payload) {
        { connection.GetTransportKind() } -> std::same_as<LikesProgram::Net::TransportKind>;
        connection.SetMaxDatagramBytes(4096);
        connection.SendTo(peer, static_cast<LikesProgram::Net::Buffer&&>(payload));
        connection.SendTo(peer, nullptr, 0);
    };

    static_assert(
        DatagramConnectionContract<LikesProgram::Net::Connection>,
        "Connection should expose peer-aware datagram configuration and send operations");
}

int main() {
    return 0;
}
