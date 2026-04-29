#pragma once
#include "Transport.hpp"

namespace LikesProgram {
    namespace Net {
        class TcpTransport : public Transport {
        public:
            explicit TcpTransport(SocketType fd) : Transport(fd) {}
            ~TcpTransport() override { Close(); }

            IoResult ReadSome(Buffer& in) override;
            IoResult WriteSome(const uint8_t* p, size_t len) override;

            void ShutdownWrite() override;
            void Close() override;
        };
    }
}
