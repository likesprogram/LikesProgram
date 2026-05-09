#include "../../../../include/LikesProgram/net/transports/TlsTransport.hpp"
#include <openssl/err.h>
#include <limits>

namespace LikesProgram::Net {
    TlsTransport::TlsTransport(SocketType fd, SSL_CTX* ctx, TlsMode mode) : Transport(fd), m_mode(mode) {
        if (!ctx) {
            m_initOk = false;
            return;
        }

        m_ssl = SSL_new(ctx);

        if (!m_ssl) {
            m_initOk = false;
            return;
        }

        if (SSL_set_fd(m_ssl, fd) != 1) {
            SSL_free(m_ssl);
            m_ssl = nullptr;
            m_initOk = false;
            return;
        }

        if (mode == TlsMode::Server) SSL_set_accept_state(m_ssl);
        else SSL_set_connect_state(m_ssl);
        m_initOk = true;
    }

    TlsTransport::~TlsTransport() {
        Close();
    }

    bool TlsTransport::NeedHandshake() const {
        return m_initOk && !m_handshakeDone;
    }

    bool TlsTransport::RemainWantRead() const {
        return m_wantRead;
    }

    bool TlsTransport::RemainWantWrite() const {
        return m_wantWrite;
    }

    IoResult TlsTransport::Handshake() {
        if (!m_initOk || !m_ssl) return MakeError(0);

        if (m_handshakeDone) return MakeOk(0);

        m_wantRead = false;
        m_wantWrite = false;

        int ret = SSL_do_handshake(m_ssl);
        if (ret == 1) {
            m_handshakeDone = true;
            return MakeOk(0);
        }

        return TranslateSslResult(ret);
    }

    IoResult TlsTransport::ReadSome(Buffer& in) {
        if (!m_initOk || !m_ssl) return MakeError(0);

        uint8_t temp[8192];

        int ret = SSL_read(m_ssl, temp, sizeof(temp));
        if (ret > 0) {
            in.Append(reinterpret_cast<const char*>(temp), static_cast<size_t>(ret));
            return MakeOk(ret);
        }

        return TranslateSslResult(ret);
    }

    IoResult TlsTransport::WriteSome(const uint8_t* p, size_t len) {
        if (!m_initOk || !m_ssl || !p) return MakeError(0);
        if (len == 0) return MakeOk(0);

        size_t writeLen = std::min<size_t>(len, static_cast<size_t>(std::numeric_limits<int>::max()));
        int ret = SSL_write(m_ssl, p, static_cast<int>(writeLen));
        if (ret > 0) return MakeOk(ret);

        return TranslateSslResult(ret);
    }

    IoResult TlsTransport::TranslateSslResult(int ret) {
        int err = SSL_get_error(m_ssl, ret);

        switch (err) {
        case SSL_ERROR_WANT_READ:
            m_wantRead = true;
            m_wantWrite = false;
            return MakeWouldBlock();
        case SSL_ERROR_WANT_WRITE:
            m_wantRead = false;
            m_wantWrite = true;
            return MakeWouldBlock();
        case SSL_ERROR_ZERO_RETURN:
            return MakePeerClosed();
        case SSL_ERROR_SYSCALL: {
            unsigned long sslErr = ERR_get_error();
            if (sslErr != 0) return MakeError(static_cast<int>(sslErr));

            int sockErr = GetSockErr();
            if (sockErr != 0) return MakeError(sockErr);

            return MakePeerClosed();
        }
        case SSL_ERROR_SSL: {
            unsigned long sslErr = ERR_get_error();
            return MakeError(sslErr != 0 ? static_cast<int>(sslErr) : err);
        }
        default: return MakeError(err);
        }
    }

    void TlsTransport::ShutdownWrite() {
        if (!m_ssl) return;
        SSL_shutdown(m_ssl);
    }

    void TlsTransport::Close() {
        bool expected = false;
        if (!m_closed.compare_exchange_strong(expected, true)) return;

        if (m_ssl) {
            SSL_shutdown(m_ssl);
            SSL_free(m_ssl);
            m_ssl = nullptr;
        }

        if (m_fd != kInvalidSocket) {
            CloseSocket(m_fd);
            m_fd = kInvalidSocket;
        }
    }

}