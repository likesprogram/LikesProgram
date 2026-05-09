#pragma once
#include "Transport.hpp"
#include <openssl/ssl.h>

namespace LikesProgram::Net {

    enum class TlsMode {
        Server,
        Client
    };

    class TlsTransport final : public Transport {
    public:
        TlsTransport(SocketType fd, SSL_CTX* ctx, TlsMode mode);
        ~TlsTransport() override;

        IoResult ReadSome(Buffer& in) override;
        IoResult WriteSome(const uint8_t* p, size_t len) override;

        void ShutdownWrite() override;
        void Close() override;

        bool NeedHandshake() const override;
        IoResult Handshake() override;

        bool RemainWantRead() const override;
        bool RemainWantWrite() const override;

    private:
        IoResult TranslateSslResult(int ret);

    private:
        SSL* m_ssl = nullptr;
        TlsMode m_mode;
        bool m_handshakeDone = false;
        bool m_initOk = false;
        bool m_wantRead = true;
        bool m_wantWrite = false;
    };

}