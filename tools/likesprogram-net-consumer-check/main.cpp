#include <LikesProgram/Net/Net.hpp>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <memory>

namespace {
    class ConsumerSecureTcpTransport final : public LikesProgram::Net::TcpTransport {
    public:
        // 外部消费方继承 TCP transport 扩展点，不需要 Net 提供 OpenSSL 实现。
        explicit ConsumerSecureTcpTransport(LikesProgram::Net::SocketType fd)
            : TcpTransport(fd) {
        }

        // 初始化每连接安全层资源，外部消费方可在这里绑定自有 TLS 会话。
        LikesProgram::Net::IoResult InitializeSecureLayer() override {
            return LikesProgram::Net::IoResult{ LikesProgram::Net::IoStatus::Ok, 0, 0 };
        }
        // 用户主动升级通信时进入自定义安全层，本检查只验证扩展点可用。
        LikesProgram::Net::IoResult UpgradeCommunication() override {
            return LikesProgram::Net::IoResult{ LikesProgram::Net::IoStatus::Ok, 0, 0 };
        }
        // 当前检查不需要握手态，真实 TLS 实现可按状态返回 true。
        bool NeedHandshake() const override { return false; }
        // 推进安全层握手，外部实现可在这里调用 SSL_do_handshake 等能力。
        LikesProgram::Net::IoResult Handshake() override {
            return LikesProgram::Net::IoResult{ LikesProgram::Net::IoStatus::Ok, 0, 0 };
        }
        // 模拟安全层仍有读兴趣，验证接口可被外部工程覆盖。
        bool RemainWantRead() const override { return true; }
        // 当前检查不保留写兴趣，避免示例引入额外状态。
        bool RemainWantWrite() const override { return false; }

    protected:
        // 明文 socket 钩子仍可由派生类接管，公共 ReadSome/WriteSome 保持框架统一调度。
        LikesProgram::Net::IoResult ReadSocketSome(LikesProgram::Net::Buffer&) override {
            return LikesProgram::Net::IoResult{ LikesProgram::Net::IoStatus::WouldBlock, 0, 0 };
        }
        LikesProgram::Net::IoResult WriteSocketSome(const std::uint8_t*, std::size_t len) override {
            return LikesProgram::Net::IoResult{
                LikesProgram::Net::IoStatus::Ok,
                static_cast<std::int64_t>(len),
                0
            };
        }
        void ShutdownSocketWrite() override {}
        void CloseSocket() override { (void)DetachFd(); }
    };

    class ConsumerSecureUdpTransport final : public LikesProgram::Net::UdpTransport {
    public:
        // 外部消费方继承 UDP transport 扩展点，可在应用侧封装自定义安全层。
        explicit ConsumerSecureUdpTransport(LikesProgram::Net::SocketType fd)
            : UdpTransport(fd) {
        }

        // 初始化每连接安全层资源，UDP 场景可在此准备 DTLS 会话。
        LikesProgram::Net::IoResult InitializeSecureLayer() override {
            return LikesProgram::Net::IoResult{ LikesProgram::Net::IoStatus::Ok, 0, 0 };
        }
        // 用户主动升级 UDP 通信时进入自定义安全层握手。
        LikesProgram::Net::IoResult UpgradeCommunication() override {
            return LikesProgram::Net::IoResult{ LikesProgram::Net::IoStatus::Ok, 0, 0 };
        }
        // 当前检查不进入握手态，只确认外部覆盖点可见。
        bool NeedHandshake() const override { return false; }
        // 推进 UDP/DTLS 风格握手，真实实现由消费方提供。
        LikesProgram::Net::IoResult Handshake() override {
            return LikesProgram::Net::IoResult{ LikesProgram::Net::IoStatus::Ok, 0, 0 };
        }
        // 保留读兴趣用于覆盖 secure transport 查询接口。
        bool RemainWantRead() const override { return true; }
        // 不保留写兴趣，避免检查程序依赖平台 socket 状态。
        bool RemainWantWrite() const override { return false; }

    protected:
        // 明文 UDP socket 钩子保持可继承，安全层只在用户主动升级后接管。
        LikesProgram::Net::IoResult ReadSocketSome(LikesProgram::Net::Buffer&) override {
            return LikesProgram::Net::IoResult{ LikesProgram::Net::IoStatus::WouldBlock, 0, 0 };
        }
        LikesProgram::Net::IoResult WriteSocketSome(const std::uint8_t*, std::size_t len) override {
            return LikesProgram::Net::IoResult{
                LikesProgram::Net::IoStatus::Ok,
                static_cast<std::int64_t>(len),
                0
            };
        }
        void ShutdownSocketWrite() override {}
        void CloseSocket() override { (void)DetachFd(); }
    };
}

int main() {
    LikesProgram::Net::Buffer buffer;
    buffer.Append("net", 3);

    ConsumerSecureTcpTransport secureTcp(LikesProgram::Net::kInvalidSocket);
    ConsumerSecureUdpTransport secureUdp(LikesProgram::Net::kInvalidSocket);

    if (secureTcp.Kind() != LikesProgram::Net::TransportKind::Tcp) return 1;
    if (secureUdp.Kind() != LikesProgram::Net::TransportKind::Udp) return 2;
    if (buffer.AsStringView() != "net") return 3;
    if (secureTcp.InitializeSecureLayer().status != LikesProgram::Net::IoStatus::Ok) return 4;
    if (secureUdp.UpgradeCommunication().status != LikesProgram::Net::IoStatus::Ok) return 5;

    std::atomic<int> sharedInitCount{ 0 }; // 验证共享安全资源初始化只随工厂状态执行一次
    LikesProgram::Net::ConnectionFactory factory(
        [](LikesProgram::Net::SocketType, LikesProgram::Net::EventLoop*) {
            return std::shared_ptr<LikesProgram::Net::Connection>{};
        },
        [&sharedInitCount]() {
            sharedInitCount.fetch_add(1);
            return true;
        });

    if (!factory.InitializeSharedSecureResources()) return 6;
    if (!factory.InitializeSharedSecureResources()) return 7;
    if (sharedInitCount.load() != 1) return 8;

    const std::uint8_t payload[] = { 'o', 'k' }; // 通过公共入口确认默认仍走明文 socket 钩子
    if (secureTcp.WriteSome(payload, sizeof(payload)).nbytes != 2) return 9;
    if (secureUdp.WriteSome(payload, sizeof(payload)).nbytes != 2) return 10;

    std::cout << LikesProgram::Net::PackageName()
        << " consumer check passed\n";
    return 0;
}
