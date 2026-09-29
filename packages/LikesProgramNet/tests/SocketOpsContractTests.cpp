#include "net/platform/SocketOps.hpp"

#include <fcntl.h>
#include <netinet/in.h>
#include <stdexcept>
#include <sys/socket.h>

namespace {
    using LikesProgram::Net::SocketLength;
    using LikesProgram::Net::SocketType;
    namespace Internal = LikesProgram::Net::Internal;

    // 在失败时保留明确的平台契约名称。
    void Require(bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    }

    // 关闭测试独占的 socket，避免异常路径泄漏句柄。
    class SocketOwner final {
    public:
        explicit SocketOwner(SocketType fd) noexcept
            : m_fd(fd) {
        }

        ~SocketOwner() {
            Internal::CloseSocket(m_fd);
        }

        SocketOwner(const SocketOwner&) = delete;
        SocketOwner& operator=(const SocketOwner&) = delete;

        SocketType Get() const noexcept {
            return m_fd;
        }

    private:
        SocketType m_fd; // 当前测试独占的 socket
    };

    // 验证通用源码所需的窄 socket 操作完整保持 Linux 行为。
    void TestLoopbackBindListenConnectAccept() {
        SocketOwner listener(Internal::CreateSocket(AF_INET, SOCK_STREAM, IPPROTO_TCP)); // 回环 listener
        Require(listener.Get() != LikesProgram::Net::kInvalidSocket,
            "SocketOps listener should open");
        Require(Internal::SetReuseAddress(listener.Get()),
            "SocketOps listener should enable address reuse");

        sockaddr_in requested{}; // 由内核分配临时回环端口
        requested.sin_family = AF_INET;
        requested.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        requested.sin_port = 0;
        Require(Internal::BindSocket(
            listener.Get(),
            reinterpret_cast<const sockaddr*>(&requested),
            static_cast<SocketLength>(sizeof(requested))) == 0,
            "BindSocket should bind loopback listener");
        Require(Internal::ListenSocket(listener.Get(), 8) == 0,
            "ListenSocket should start loopback listener");

        sockaddr_in bound{}; // 内核实际选择的稳定端口
        SocketLength boundLength = static_cast<SocketLength>(sizeof(bound));
        Require(::getsockname(
            listener.Get(),
            reinterpret_cast<sockaddr*>(&bound),
            &boundLength) == 0,
            "SocketOps listener should expose bound address");

        SocketOwner client(Internal::CreateSocket(AF_INET, SOCK_STREAM, IPPROTO_TCP)); // 阻塞连接确保 accept 已就绪
        Require(client.Get() != LikesProgram::Net::kInvalidSocket,
            "SocketOps client should open");
        Require(Internal::ConnectSocket(
            client.Get(),
            reinterpret_cast<const sockaddr*>(&bound),
            boundLength) == 0,
            "ConnectSocket should complete loopback connect");

        SocketOwner accepted(Internal::AcceptSocket(listener.Get(), nullptr, nullptr)); // helper 接管平台 accept 细节
        Require(accepted.Get() != LikesProgram::Net::kInvalidSocket,
            "AcceptSocket should return a client");
        const int acceptedFlags = ::fcntl(accepted.Get(), F_GETFL, 0); // 读取 helper 设置的非阻塞属性
        Require(acceptedFlags >= 0 && (acceptedFlags & O_NONBLOCK) != 0,
            "AcceptSocket should return a nonblocking socket");
        Require(Internal::SupportsWorkerLocalListenerReuse(),
            "Linux should support worker-local listener reuse");
    }
}

// 执行平台 socket 窄接口契约。
int main() {
    TestLoopbackBindListenConnectAccept();
    return 0;
}
