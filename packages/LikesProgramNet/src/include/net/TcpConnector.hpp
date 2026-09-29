#pragma once
#include <LikesProgram/Net/Address.hpp>
#include <LikesProgram/Net/Poller.hpp>

namespace LikesProgram {
    namespace Net {
        class EventLoop;

        namespace Internal {
            class TcpConnector {
            public:
                // 在 issuer 线程提交 TCP connect completion。
                static Poller::ConnectId Start(
                    EventLoop* loop,
                    const Address& remoteAddress,
                    Poller::ConnectCallback callback) noexcept;
                // 在 issuer 线程取消尚未完成的 TCP connect completion。
                static void Cancel(EventLoop* loop, Poller::ConnectId connectId) noexcept;
            };
        }
    }
}
