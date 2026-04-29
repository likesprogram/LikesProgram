//#pragma once
//#include <string>
//#include <unordered_map>
//#include <shared_mutex>
//#include <memory>
//#include <openssl/ssl.h>
//
//namespace LikesProgram {
//    namespace Net {
//        class TlsContextManager {
//        public:
//            TlsContextManager();
//            ~TlsContextManager();
//
//            bool LoadDefault(const std::string& cert, const std::string& key);
//
//            bool AddCertificate(const std::string& domain,
//                const std::string& cert,
//                const std::string& key);
//
//            SSL_CTX* GetDefault() const;
//
//            // SNI 使用（内部用）
//            SSL_CTX* SelectByDomain(const std::string& domain) const;
//
//            // OpenSSL SNI 回调入口
//            static int SniCallback(SSL* ssl, int* ad, void* arg);
//
//        private:
//            SSLContextPtr CreateContext(const std::string& cert, const std::string& key);
//
//        private:
//            SSLContextPtr m_default;
//
//            std::unordered_map<std::string, SSLContextPtr> m_sniMap;
//
//            mutable std::shared_mutex m_mutex;
//        };
//    }
//}
