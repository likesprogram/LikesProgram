#if defined(__linux__)

#include <liburing.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <unordered_set>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
    constexpr std::string_view kHttpResponse =
        "HTTP/1.1 200 OK\r\n"
        "Server: LikesProgramNet\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 16\r\n"
        "Connection: keep-alive\r\n"
        "\r\n"
        "LikesProgramNet\n";

    constexpr std::array<char, 4> kHeaderEnd = { '\r', '\n', '\r', '\n' };
    constexpr std::size_t kInputCapacity = 8 * 1024;
    constexpr unsigned int kRingEntries = 4096;

    std::atomic<bool> g_running{ true }; // 信号处理函数只切换 probe 运行状态

    enum class OperationKind {
        Accept,
        Receive,
        Send
    };

    struct Operation {
        OperationKind kind = OperationKind::Accept; // completion 对应的操作类别
        void* owner = nullptr;                      // Worker 或 Connection，不拥有生命周期
    };

    struct Connection {
        explicit Connection(int socketFd)
            : fd(socketFd) {
            operation.owner = this;
        }

        int fd = -1;                                // 当前 worker 独占的客户端 socket
        Operation operation{ OperationKind::Receive, nullptr }; // 同一连接只保留一个在途操作
        std::array<char, kInputCapacity> input{};    // HTTP header 增量接收缓冲
        std::size_t inputSize = 0;                   // 已接收且尚未消费的字节数
        std::size_t sendOffset = 0;                  // 固定响应部分写入偏移
    };

    void HandleSignal(int) {
        g_running.store(false, std::memory_order_release);
    }

    bool SetNonBlocking(int fd) noexcept {
        const int flags = ::fcntl(fd, F_GETFL, 0); // 保留已有 fd 标志
        return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
    }

    bool SetTcpNoDelay(int fd) noexcept {
        const int enabled = 1; // 固定小响应避免 Nagle 延迟
        return ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) == 0;
    }

    int CreateListener(std::uint16_t port) {
        const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
        if (fd < 0) throw std::runtime_error("io_uring probe socket failed");

        const int enabled = 1; // 每个 worker 使用独立 SO_REUSEPORT listener
        if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) != 0
            || ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &enabled, sizeof(enabled)) != 0
            || !SetNonBlocking(fd)) {
            (void)::close(fd);
            throw std::runtime_error("io_uring probe socket options failed");
        }

        sockaddr_in address{}; // probe 只使用本机回环公平对标现有 wrk 口径
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0
            || ::listen(fd, SOMAXCONN) != 0) {
            (void)::close(fd);
            throw std::runtime_error("io_uring probe bind/listen failed");
        }
        return fd;
    }

    class Worker {
    public:
        explicit Worker(std::uint16_t port)
            : m_listenFd(CreateListener(port)) {
            io_uring_params parameters{}; // 默认 ring 即可使用内核 FAST_POLL
            if (::io_uring_queue_init_params(kRingEntries, &m_ring, &parameters) < 0) {
                (void)::close(m_listenFd);
                m_listenFd = -1;
                throw std::runtime_error("io_uring queue init failed");
            }
            m_ringReady = true;
            m_acceptOperation.kind = OperationKind::Accept;
            m_acceptOperation.owner = this;
            QueueAccept();
            Submit();
        }

        ~Worker() {
            ShutdownConnections();
            if (m_ringReady) ::io_uring_queue_exit(&m_ring);
            if (m_listenFd >= 0) (void)::close(m_listenFd);
        }

        Worker(const Worker&) = delete;
        Worker& operator=(const Worker&) = delete;

        void Run() {
            while (g_running.load(std::memory_order_acquire)) {
                __kernel_timespec timeout{}; // 周期检查退出信号，不依赖额外 eventfd
                timeout.tv_nsec = 100 * 1000 * 1000;

                io_uring_cqe* completion = nullptr; // 本轮至少等待一个 completion
                const int waitResult = ::io_uring_wait_cqe_timeout(&m_ring, &completion, &timeout);
                if (waitResult == -ETIME || waitResult == -EINTR) continue;
                if (waitResult < 0) throw std::runtime_error("io_uring wait failed");

                do {
                    HandleCompletion(completion);
                    ::io_uring_cqe_seen(&m_ring, completion);
                    completion = nullptr;
                } while (::io_uring_peek_cqe(&m_ring, &completion) == 0);

                // 将本批 completion 产生的 recv/send/accept 一次提交给内核。
                Submit();
            }
        }

    private:
        io_uring_sqe* NextSubmission() {
            io_uring_sqe* submission = ::io_uring_get_sqe(&m_ring); // 优先复用当前 SQ
            if (submission != nullptr) return submission;

            Submit();
            submission = ::io_uring_get_sqe(&m_ring);
            if (submission == nullptr) throw std::runtime_error("io_uring SQ is full");
            return submission;
        }

        void Submit() {
            const int result = ::io_uring_submit(&m_ring); // 空 SQ 时 liburing 会直接返回 0
            if (result < 0) throw std::runtime_error("io_uring submit failed");
        }

        void QueueAccept() {
            io_uring_sqe* submission = NextSubmission(); // multishot accept 批量接收新连接
            ::io_uring_prep_multishot_accept(
                submission,
                m_listenFd,
                nullptr,
                nullptr,
                SOCK_NONBLOCK | SOCK_CLOEXEC);
            ::io_uring_sqe_set_data(submission, &m_acceptOperation);
        }

        void QueueReceive(Connection* connection) {
            if (connection == nullptr || connection->inputSize >= connection->input.size()) {
                CloseConnection(connection);
                return;
            }

            connection->operation.kind = OperationKind::Receive;
            io_uring_sqe* submission = NextSubmission(); // 内核等待可读后直接写入连接缓冲
            ::io_uring_prep_recv(
                submission,
                connection->fd,
                connection->input.data() + connection->inputSize,
                connection->input.size() - connection->inputSize,
                0);
            ::io_uring_sqe_set_data(submission, &connection->operation);
        }

        void QueueSend(Connection* connection) {
            connection->operation.kind = OperationKind::Send;
            io_uring_sqe* submission = NextSubmission(); // 固定响应允许复用只读静态内存
            ::io_uring_prep_send(
                submission,
                connection->fd,
                kHttpResponse.data() + connection->sendOffset,
                kHttpResponse.size() - connection->sendOffset,
                MSG_NOSIGNAL);
            ::io_uring_sqe_set_data(submission, &connection->operation);
        }

        bool ConsumeOneRequest(Connection* connection) {
            const char* begin = connection->input.data(); // 只扫描当前未消费 header
            const char* end = begin + connection->inputSize;
            const char* headerEnd = std::search(begin, end, kHeaderEnd.begin(), kHeaderEnd.end());
            if (headerEnd == end) return false;

            const std::size_t consumed = static_cast<std::size_t>(headerEnd - begin) + kHeaderEnd.size();
            const std::size_t remaining = connection->inputSize - consumed; // 保留 pipeline 后续请求
            if (remaining > 0) std::memmove(connection->input.data(), begin + consumed, remaining);
            connection->inputSize = remaining;
            return true;
        }

        void QueueNextApplicationStep(Connection* connection) {
            if (ConsumeOneRequest(connection)) {
                connection->sendOffset = 0;
                QueueSend(connection);
            }
            else {
                QueueReceive(connection);
            }
        }

        void HandleCompletion(io_uring_cqe* completion) {
            auto* operation = static_cast<Operation*>(::io_uring_cqe_get_data(completion));
            if (operation == nullptr) return;

            if (operation->kind == OperationKind::Accept) {
                HandleAccept(completion);
                return;
            }

            auto* connection = static_cast<Connection*>(operation->owner); // 在途操作持有连接生命周期
            if (connection == nullptr || m_connections.find(connection) == m_connections.end()) return;
            if (operation->kind == OperationKind::Receive) {
                HandleReceive(connection, completion->res);
            }
            else {
                HandleSend(connection, completion->res);
            }
        }

        void HandleAccept(io_uring_cqe* completion) {
            if (completion->res >= 0) {
                const int clientFd = completion->res; // completion 直接返回已接受 socket
                (void)SetTcpNoDelay(clientFd);
                auto* connection = new Connection(clientFd);
                m_connections.insert(connection);
                QueueReceive(connection);
            }

            if ((completion->flags & IORING_CQE_F_MORE) == 0
                && g_running.load(std::memory_order_acquire)) {
                // multishot 被内核终止后重新挂接，避免 listener 静默失活。
                QueueAccept();
            }
        }

        void HandleReceive(Connection* connection, int result) {
            if (result <= 0) {
                CloseConnection(connection);
                return;
            }

            connection->inputSize += static_cast<std::size_t>(result);
            QueueNextApplicationStep(connection);
        }

        void HandleSend(Connection* connection, int result) {
            if (result <= 0) {
                CloseConnection(connection);
                return;
            }

            connection->sendOffset += static_cast<std::size_t>(result);
            if (connection->sendOffset < kHttpResponse.size()) {
                QueueSend(connection);
                return;
            }

            QueueNextApplicationStep(connection);
        }

        void CloseConnection(Connection* connection) noexcept {
            if (connection == nullptr) return;
            const auto found = m_connections.find(connection); // 防止同一 completion 重复关闭
            if (found == m_connections.end()) return;

            m_connections.erase(found);
            if (connection->fd >= 0) (void)::close(connection->fd);
            delete connection;
        }

        void ShutdownConnections() noexcept {
            for (Connection* connection : m_connections) {
                if (connection != nullptr && connection->fd >= 0) (void)::close(connection->fd);
                delete connection;
            }
            m_connections.clear();
        }

        io_uring m_ring{};                          // 当前 worker 独占的 completion ring
        bool m_ringReady = false;                   // queue_exit 生命周期闸门
        int m_listenFd = -1;                        // SO_REUSEPORT listener
        Operation m_acceptOperation{};              // multishot accept 的稳定 user_data
        std::unordered_set<Connection*> m_connections; // 仅 worker 线程访问的活跃连接集合
    };

    std::uint16_t ParsePort(int argc, char** argv) {
        if (argc < 2) return 18090;
        const long value = std::strtol(argv[1], nullptr, 10); // probe 只接受有效 TCP 端口
        if (value <= 0 || value > 65535) throw std::runtime_error("invalid probe port");
        return static_cast<std::uint16_t>(value);
    }

    unsigned int ParseWorkers(int argc, char** argv) {
        if (argc < 3) return std::max(1u, std::thread::hardware_concurrency());
        const long value = std::strtol(argv[2], nullptr, 10); // worker 数用于和 Net/Nginx 同口径 A/B
        if (value <= 0 || value > 1024) throw std::runtime_error("invalid probe workers");
        return static_cast<unsigned int>(value);
    }
}

int main(int argc, char** argv) {
    try {
        std::signal(SIGINT, HandleSignal);
        std::signal(SIGTERM, HandleSignal);

        const std::uint16_t port = ParsePort(argc, argv); // 所有 worker 通过 REUSEPORT 监听同一端口
        const unsigned int workerCount = ParseWorkers(argc, argv);
        std::vector<std::unique_ptr<Worker>> workers; // worker/ring 生命周期由主线程统一管理
        std::vector<std::thread> threads;             // 每个 ring 固定一个 completion 消费线程
        workers.reserve(workerCount);
        threads.reserve(workerCount);

        for (unsigned int index = 0; index < workerCount; ++index) {
            workers.push_back(std::make_unique<Worker>(port));
        }
        for (const auto& worker : workers) {
            threads.emplace_back([instance = worker.get()]() { instance->Run(); });
        }

        std::cout << "io_uring_http_probe ready"
            << " port=" << port
            << " workers=" << workerCount
            << '\n';
        std::cout.flush();

        while (g_running.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        for (std::thread& thread : threads) {
            if (thread.joinable()) thread.join();
        }
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "IoUringHttpProbe failed: " << error.what() << '\n';
        return 1;
    }
}

#else

int main() {
    return 77;
}

#endif
