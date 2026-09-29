#include "net/TcpConnector.hpp"
#include <LikesProgram/Net/EventLoop.hpp>
#include <utility>

namespace LikesProgram {
    namespace Net {
        namespace Internal {
            Poller::ConnectId TcpConnector::Start(
                EventLoop* loop,
                const Address& remoteAddress,
                Poller::ConnectCallback callback) noexcept {
                if (loop == nullptr || !callback) return Poller::InvalidConnectId;
                return loop->PollerRef().StartConnect(remoteAddress, std::move(callback));
            }

            void TcpConnector::Cancel(EventLoop* loop, Poller::ConnectId connectId) noexcept {
                if (loop == nullptr || connectId == Poller::InvalidConnectId) return;
                loop->PollerRef().CancelConnect(connectId);
            }
        }
    }
}
