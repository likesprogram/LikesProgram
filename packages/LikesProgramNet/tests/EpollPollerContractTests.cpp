#include "net/platform/linux/EpollDriver.hpp"

#include <LikesProgram/Net/Address.hpp>
#include <LikesProgram/Net/Channel.hpp>
#include <LikesProgram/Net/EventLoop.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <iostream>
#include <memory>
#include <netinet/in.h>
#include <pthread.h>
#include <set>
#include <stdexcept>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
    using LikesProgram::Net::Channel;
    using LikesProgram::Net::EventLoop;
    using LikesProgram::Net::IOEvent;
    using LikesProgram::Net::Internal::EpollDriver;

    // 在断言失败时保留明确的契约名称。
    void Require(bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    }

    // 信号仅用于打断 epoll_wait，不改变测试状态。
    void IgnorePollInterruptSignal(int signalNumber) {
        (void)signalNumber;
    }

    class SocketPair final {
    public:
        // 创建非阻塞、close-on-exec 的本地 stream socket 对。
        SocketPair() {
            if (::socketpair(
                AF_UNIX,
                SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                0,
                m_fds) != 0) {
                throw std::runtime_error("socketpair should open");
            }
        }

        // 释放测试持有的两个 socket。
        ~SocketPair() {
            CloseEndpoint(0);
            CloseEndpoint(1);
        }

        SocketPair(const SocketPair&) = delete;
        SocketPair& operator=(const SocketPair&) = delete;

        // 返回指定端点的文件描述符。
        int Endpoint(std::size_t index) const noexcept {
            return index < 2 ? m_fds[index] : -1;
        }

        // 关闭并释放指定端点。
        void CloseEndpoint(std::size_t index) noexcept {
            if (index >= 2 || m_fds[index] < 0) return;
            (void)::close(m_fds[index]);
            m_fds[index] = -1;
        }

        // 把首端点复制到已释放的历史 fd，制造确定性 fd 复用。
        void ReuseFirstEndpointAs(int targetFd) {
            if (m_fds[0] == targetFd) return;
            Require(::dup2(m_fds[0], targetFd) == targetFd,
                "dup2 should reuse the removed Channel fd");
            CloseEndpoint(0);
            m_fds[0] = targetFd;
        }

    private:
        int m_fds[2] = { -1, -1 }; // 当前测试独占的 socket 端点
    };

    class LoopbackListener final {
    public:
        // 创建由内核分配端口的非阻塞 loopback listener。
        LoopbackListener()
            : m_fd(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP)) {
            try {
                Require(m_fd >= 0, "Loopback listener socket should open");
                int reuseAddress = 1; // 允许失败用例快速复跑临时端口
                Require(::setsockopt(
                    m_fd,
                    SOL_SOCKET,
                    SO_REUSEADDR,
                    &reuseAddress,
                    sizeof(reuseAddress)) == 0,
                    "Loopback listener should enable address reuse");

                sockaddr_in address{}; // 内核选择 loopback 临时端口
                address.sin_family = AF_INET;
                address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                address.sin_port = 0;
                Require(::bind(
                    m_fd,
                    reinterpret_cast<const sockaddr*>(&address),
                    sizeof(address)) == 0,
                    "Loopback listener should bind");
                Require(::listen(m_fd, SOMAXCONN) == 0, "Loopback listener should listen");
                socklen_t addressLength = sizeof(address); // 读取内核分配的端口
                Require(::getsockname(
                    m_fd,
                    reinterpret_cast<sockaddr*>(&address),
                    &addressLength) == 0,
                    "Loopback listener should expose its bound port");
                m_address = LikesProgram::Net::Address("127.0.0.1", ntohs(address.sin_port));
            }
            catch (...) {
                if (m_fd >= 0) (void)::close(m_fd);
                m_fd = -1;
                throw;
            }
        }

        // 关闭 listener 并释放尚未 accept 的内核队列。
        ~LoopbackListener() {
            if (m_fd >= 0) (void)::close(m_fd);
            m_fd = -1;
        }

        LoopbackListener(const LoopbackListener&) = delete;
        LoopbackListener& operator=(const LoopbackListener&) = delete;

        // 返回 listener fd。
        int Fd() const noexcept {
            return m_fd;
        }

        // 返回 connect 使用的稳定 loopback 地址。
        const LikesProgram::Net::Address& Address() const noexcept {
            return m_address;
        }

        // 创建一个阻塞 client 并完成 loopback 三次握手。
        int ConnectClient() const {
            const int clientFd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP); // 调用方接管
            Require(clientFd >= 0, "Loopback client socket should open");
            if (::connect(clientFd, m_address.SockAddr(), m_address.Length()) != 0) {
                (void)::close(clientFd);
                throw std::runtime_error("Loopback client should connect");
            }
            return clientFd;
        }

    private:
        int m_fd = -1; // 当前测试独占的 listener fd
        LikesProgram::Net::Address m_address; // 内核选择的 loopback 地址快照
    };

    // 返回当前进程稳定打开的 fd 集合，并排除扫描目录自身。
    std::set<int> OpenDescriptors() {
        DIR* directory = ::opendir("/proc/self/fd"); // Linux fd 生命周期观察入口
        Require(directory != nullptr, "The fd contract should open /proc/self/fd");
        const int directoryFd = ::dirfd(directory); // 每次扫描临时产生的 fd 不参与差异
        std::set<int> descriptors; // 调用时刻的稳定 fd 数值集合
        while (dirent* entry = ::readdir(directory)) {
            char* end = nullptr; // strtol 完整消费检查
            const long value = std::strtol(entry->d_name, &end, 10);
            if (end == entry->d_name || *end != '\0' || value == directoryFd || value < 0) continue;
            descriptors.insert(static_cast<int>(value));
        }
        (void)::closedir(directory);
        return descriptors;
    }

    // 找出一次 StartConnect 唯一新增的 socket fd。
    int SingleAddedDescriptor(
        const std::set<int>& before,
        const std::set<int>& after) {
        int addedDescriptor = -1; // connect state 应只拥有一个新增 socket
        for (int descriptor : after) {
            if (before.find(descriptor) != before.end()) continue;
            Require(addedDescriptor < 0, "StartConnect should add exactly one socket fd");
            addedDescriptor = descriptor;
        }
        Require(addedDescriptor >= 0, "StartConnect should own one socket fd");
        return addedDescriptor;
    }

    // 分派 Poller 返回的 Channel completion。
    void DispatchActiveChannels(std::vector<Channel*>& activeChannels) {
        for (Channel* channel : activeChannels) {
            if (channel != nullptr) channel->HandleEvent();
        }
    }

    // 验证 registration id 不会因 fd 复用把旧事件投递给新 Channel。
    void TestEpollChannelUpdateRemoveAndFdReuse() {
        EpollDriver poller(nullptr); // 直接隔离 epoll Channel registration
        Require(poller.Activate(), "EpollDriver should activate for Channel contracts");

        SocketPair oldSockets; // 首个 Channel 用于预留迟到的旧 registration 事件
        SocketPair newSockets; // 第二个 Channel 在旧 fd 删除后复用相同数值
        const int reusedFd = oldSockets.Endpoint(0); // 必须复用的历史 Channel fd
        int oldReadCallbacks = 0; // 旧 registration 不得在删除后回调
        Channel oldChannel(nullptr, reusedFd, IOEvent::Read);
        oldChannel.SetReadCallback([&oldReadCallbacks]() { ++oldReadCallbacks; });
        Require(poller.AddChannel(&oldChannel), "EpollDriver should add the first Channel");

        const char oldByte = 'o'; // 在删除前让内核产生旧 registration readiness
        Require(::send(oldSockets.Endpoint(1), &oldByte, 1, 0) == 1,
            "Old Channel peer should queue one readable byte");
        Require(poller.RemoveChannel(&oldChannel), "EpollDriver should remove the first Channel");
        oldSockets.CloseEndpoint(0);
        newSockets.ReuseFirstEndpointAs(reusedFd);

        int newReadCallbacks = 0; // 新 Channel 只消费自身 peer 的 readiness
        int newWriteCallbacks = 0; // UpdateChannel 必须同步新增写关注
        Channel newChannel(nullptr, reusedFd, IOEvent::Read);
        newChannel.SetReadCallback([&newReadCallbacks, reusedFd]() {
            char byte = 0; // drain level-triggered readable state
            if (::recv(reusedFd, &byte, 1, 0) == 1) ++newReadCallbacks;
        });
        newChannel.SetWriteCallback([&newWriteCallbacks]() { ++newWriteCallbacks; });
        Require(poller.AddChannel(&newChannel), "EpollDriver should add the fd-reuse Channel");

        std::vector<Channel*> activeChannels; // 每轮只保存当前 registration 快照
        poller.Poll(20, activeChannels);
        DispatchActiveChannels(activeChannels);
        Require(oldReadCallbacks == 0, "Removed Channel must suppress late readiness");
        Require(newReadCallbacks == 0,
            "Old registration id must not target the fd-reuse Channel");

        const char newByte = 'n'; // 新 peer readiness 必须命中新 registration id
        Require(::send(newSockets.Endpoint(1), &newByte, 1, 0) == 1,
            "New Channel peer should queue one readable byte");
        poller.Poll(100, activeChannels);
        DispatchActiveChannels(activeChannels);
        Require(newReadCallbacks == 1, "New Channel should receive its own readiness once");

        newChannel.EnableWriting();
        Require(poller.UpdateChannel(&newChannel), "EpollDriver should update Channel interests");
        poller.Poll(100, activeChannels);
        DispatchActiveChannels(activeChannels);
        Require(newWriteCallbacks == 1, "Updated Channel should receive writable readiness");

        newChannel.DisableWriting();
        Require(poller.UpdateChannel(&newChannel), "EpollDriver should remove writable interest");
        Require(poller.RemoveChannel(&newChannel), "EpollDriver should remove the fd-reuse Channel");
        Require(!poller.HasChannel(&newChannel), "Removed Channel should leave the Poller map");
    }

    // 验证到期 callback 恰好执行一次且不伪造 Channel。
    void TestEpollTimeoutFiresOnce() {
        EpollDriver poller(nullptr); // 直接隔离 timeout heap
        Require(poller.Activate(), "EpollDriver should activate for timeout contracts");

        int callbackCount = 0; // 单个 active generation 只能完成一次
        const auto timeoutId = poller.ScheduleTimeout(
            std::chrono::milliseconds(20),
            [&callbackCount]() { ++callbackCount; });
        Require(timeoutId != LikesProgram::Net::Poller::InvalidTimeoutId,
            "Scheduled epoll timeout should return a valid id");

        std::vector<Channel*> activeChannels; // timer completion 不进入 Channel 分发
        poller.Poll(250, activeChannels);
        Require(callbackCount == 1, "Expired epoll timeout should fire exactly once");
        Require(activeChannels.empty(), "Timeout completion should not return a Channel");
        poller.Poll(30, activeChannels);
        Require(callbackCount == 1, "Expired timeout generation must not fire again");
    }

    // 验证取消只删除 active generation，旧 heap node 不得执行 callback。
    void TestEpollTimeoutCancelSuppressesCallback() {
        EpollDriver poller(nullptr); // 直接隔离取消后的 stale heap node
        Require(poller.Activate(), "EpollDriver should activate for timeout cancellation");

        bool callbackFired = false; // 取消后的 callback 必须保持 false
        const auto timeoutId = poller.ScheduleTimeout(
            std::chrono::milliseconds(20),
            [&callbackFired]() { callbackFired = true; });
        Require(timeoutId != LikesProgram::Net::Poller::InvalidTimeoutId,
            "Cancelable epoll timeout should return a valid id");
        poller.CancelTimeout(timeoutId);

        std::vector<Channel*> activeChannels; // 只等待越过原 deadline
        poller.Poll(60, activeChannels);
        Require(!callbackFired, "Canceled epoll timeout must suppress its callback");
        Require(activeChannels.empty(), "Canceled timeout should not return a Channel");
    }

    // 验证重复 EINTR 始终共享 timer 的原始绝对 deadline。
    void TestEpollTimeoutUsesSingleDeadlineAcrossEintr() {
        EpollDriver poller(nullptr); // 直接观察 epoll_wait 的 EINTR 预算
        Require(poller.Activate(), "EpollDriver should activate for EINTR timeout contracts");

        struct sigaction interruptAction{}; // 不使用 SA_RESTART，确保 epoll_wait 返回 EINTR
        interruptAction.sa_handler = IgnorePollInterruptSignal;
        (void)::sigemptyset(&interruptAction.sa_mask);
        struct sigaction previousAction{}; // 用例结束前恢复进程级信号处理器
        Require(::sigaction(SIGUSR1, &interruptAction, &previousAction) == 0,
            "EINTR contract should install the SIGUSR1 handler");

        int callbackCount = 0; // 原始 100 ms deadline 到达后只完成一次
        const auto timeoutId = poller.ScheduleTimeout(
            std::chrono::milliseconds(100),
            [&callbackCount]() { ++callbackCount; });
        const pthread_t pollingThread = ::pthread_self(); // 信号只打断当前 Poll 线程
        std::atomic<bool> pollStarting{ false }; // 辅助线程等待 Poll 即将开始
        std::atomic<int> signalFailure{ 0 }; // 保存首个 pthread_kill 错误
        std::thread interrupter([pollingThread, &pollStarting, &signalFailure]() {
            while (!pollStarting.load(std::memory_order_acquire)) std::this_thread::yield();
            for (int attempt = 0; attempt < 20; ++attempt) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                const int result = ::pthread_kill(pollingThread, SIGUSR1); // 覆盖整个 timer 等待窗口
                if (result != 0) {
                    signalFailure.store(result, std::memory_order_release);
                    break;
                }
            }
        });

        std::vector<Channel*> activeChannels; // EINTR 与 timer 均不返回 Channel
        const auto startedAt = std::chrono::steady_clock::now(); // 记录 Poll 自身耗时
        pollStarting.store(true, std::memory_order_release);
        poller.Poll(500, activeChannels);
        const auto pollElapsed = std::chrono::steady_clock::now() - startedAt; // 不包含 join 等待
        interrupter.join();
        const bool restored = ::sigaction(SIGUSR1, &previousAction, nullptr) == 0; // 先恢复再断言

        Require(timeoutId != LikesProgram::Net::Poller::InvalidTimeoutId,
            "Signal-interrupted epoll timeout should return a valid id");
        Require(signalFailure.load(std::memory_order_acquire) == 0,
            "SIGUSR1 should reach the epoll polling thread");
        Require(restored, "EINTR contract should restore the previous signal handler");
        Require(callbackCount == 1, "EINTR must not suppress or duplicate the timeout callback");
        Require(activeChannels.empty(), "Signal-interrupted timeout should not return a Channel");
        Require(pollElapsed < std::chrono::milliseconds(170),
            "EINTR must not restart the complete timeout budget");
    }

    // 验证 connect 只在 Poll issuer 线程异步完成并移交 socket。
    void TestEpollConnectCompletesOnIssuerThread() {
        LoopbackListener listener; // 为非阻塞 connect 提供稳定成功目标
        EpollDriver poller(nullptr); // 直接隔离 connect registration
        Require(poller.Activate(), "EpollDriver should activate for connect contracts");

        const pthread_t issuerThread = ::pthread_self(); // StartConnect 与 Poll 共用 issuer
        pthread_t callbackThread{}; // callback 实际执行线程
        int callbackCount = 0; // connect completion 只能执行一次
        int callbackFd = -1; // 成功 completion 移交的 socket
        int callbackError = -1; // 成功 completion 必须报告零错误
        const auto connectId = poller.StartConnect(
            listener.Address(),
            [&](int fd, int error) {
                ++callbackCount;
                callbackThread = ::pthread_self();
                callbackFd = fd;
                callbackError = error;
            });
        Require(connectId != LikesProgram::Net::Poller::InvalidConnectId,
            "Epoll connect should return a valid operation id");
        Require(callbackCount == 0, "StartConnect must not invoke callback synchronously");

        std::vector<Channel*> activeChannels; // connect completion 不伪装成 Channel
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (callbackCount == 0 && std::chrono::steady_clock::now() < deadline) {
            poller.Poll(20, activeChannels);
        }
        Require(callbackCount == 1, "Epoll connect should complete exactly once");
        Require(::pthread_equal(callbackThread, issuerThread) != 0,
            "Epoll connect callback should execute on the Poll issuer thread");
        Require(callbackError == 0 && callbackFd >= 0,
            "Epoll connect should transfer a successful socket");
        Require(activeChannels.empty(), "Connect completion should not return a Channel");
        (void)::close(callbackFd);
    }

    // 验证取消同步关闭 Poller 拥有的 socket 且不执行 callback。
    void TestEpollConnectCancelClosesSocketWithoutCallback() {
        LoopbackListener listener; // connect 可进入立即或 EINPROGRESS 路径
        EpollDriver poller(nullptr); // 直接观察 connect state 的 fd 所有权
        Require(poller.Activate(), "EpollDriver should activate for connect cancellation");

        const std::set<int> before = OpenDescriptors(); // StartConnect 前 fd 基线
        bool callbackFired = false; // 取消后必须保持 false
        const auto connectId = poller.StartConnect(
            listener.Address(),
            [&callbackFired](int fd, int error) {
                (void)fd;
                (void)error;
                callbackFired = true;
            });
        Require(connectId != LikesProgram::Net::Poller::InvalidConnectId,
            "Cancelable epoll connect should return a valid id");
        const std::set<int> during = OpenDescriptors(); // connect state 应持有唯一 socket
        (void)SingleAddedDescriptor(before, during);

        poller.CancelConnect(connectId);
        const std::set<int> after = OpenDescriptors(); // 取消应同步恢复 fd 基线
        Require(after == before, "CancelConnect should close its owned socket immediately");
        std::vector<Channel*> activeChannels; // 迟到 readiness 只能按旧 id 丢弃
        poller.Poll(30, activeChannels);
        Require(!callbackFired, "Canceled epoll connect must suppress its callback");
        Require(poller.GetCompletionStats().pendingConnectOperations == 0,
            "Canceled epoll connect should leave no pending operation");
    }

    // 验证旧 connect registration id 不能完成复用同一 fd 的新请求。
    void TestEpollConnectFdReuseIgnoresLateRegistration() {
        LoopbackListener listener; // 两个请求共享目标但必须保持 operation 身份隔离
        EpollDriver poller(nullptr); // registration id 是唯一内核身份
        Require(poller.Activate(), "EpollDriver should activate for connect fd reuse");

        const std::set<int> before = OpenDescriptors(); // 两次 connect 共用的 fd 基线
        int firstCallbacks = 0; // 已取消请求永远不回调
        const auto firstId = poller.StartConnect(
            listener.Address(),
            [&firstCallbacks](int fd, int error) {
                (void)fd;
                (void)error;
                ++firstCallbacks;
            });
        Require(firstId != LikesProgram::Net::Poller::InvalidConnectId,
            "First epoll connect should return a valid id");
        const int firstFd = SingleAddedDescriptor(before, OpenDescriptors()); // 记录旧 registration fd
        poller.CancelConnect(firstId);
        Require(OpenDescriptors() == before, "First connect cancellation should restore the fd baseline");

        int secondCallbacks = 0; // 新 registration 只完成自己的 callback
        int secondFdResult = -1; // 新请求完成后移交的 socket
        int secondError = -1; // 新请求预期成功
        const auto secondId = poller.StartConnect(
            listener.Address(),
            [&](int fd, int error) {
                ++secondCallbacks;
                secondFdResult = fd;
                secondError = error;
            });
        Require(secondId != LikesProgram::Net::Poller::InvalidConnectId,
            "Second epoll connect should return a valid id");
        const int secondOwnedFd = SingleAddedDescriptor(before, OpenDescriptors()); // OS 应复用最低空闲 fd
        Require(secondOwnedFd == firstFd, "Second connect should reuse the canceled numeric fd");

        std::vector<Channel*> activeChannels; // 同批旧事件必须按缺失 id 忽略
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (secondCallbacks == 0 && std::chrono::steady_clock::now() < deadline) {
            poller.Poll(20, activeChannels);
        }
        Require(firstCallbacks == 0, "Late old registration must not invoke the canceled callback");
        Require(secondCallbacks == 1 && secondError == 0 && secondFdResult == secondOwnedFd,
            "Fd-reuse connect should complete only the new operation");
        (void)::close(secondFdResult);
    }

    // 验证一次 level-triggered readiness 会 drain 当前三个已连接 client。
    void TestEpollAcceptDrainsMultipleClients() {
        LoopbackListener listener; // accept4 drain 的单一 listener
        EpollDriver poller(nullptr); // 直接隔离 accept registration
        Require(poller.Activate(), "EpollDriver should activate for accept contracts");

        const pthread_t issuerThread = ::pthread_self(); // accept callback 必须保持 issuer 线程
        int acceptedCount = 0; // 单轮 readiness 应消费三个 client
        bool wrongThread = false; // 任一 callback 跨线程即失败
        Require(poller.StartAccept(listener.Fd(), [&](int clientFd) {
            ++acceptedCount;
            wrongThread = wrongThread || ::pthread_equal(::pthread_self(), issuerThread) == 0;
            (void)::close(clientFd);
        }), "Epoll accept should register the listener");

        std::array<int, 3> clients{ -1, -1, -1 }; // readiness 前完整建立三条连接
        for (int& clientFd : clients) clientFd = listener.ConnectClient();
        std::vector<Channel*> activeChannels; // accept completion 不进入 Channel 分发
        poller.Poll(250, activeChannels);
        Require(acceptedCount == 3, "One epoll readiness should drain all queued clients");
        Require(!wrongThread, "Epoll accept callbacks should execute on the Poll issuer thread");
        Require(poller.AcceptSubmissionCount() == 1,
            "One live epoll listener registration should count as one accept submission");
        poller.StopAccept(listener.Fd());
        for (int clientFd : clients) (void)::close(clientFd);
    }

    // 验证 StopAccept 先撤销 id 后抑制同批迟到 readiness。
    void TestEpollStopAcceptSuppressesLateEvents() {
        LoopbackListener listener; // client 在停止前制造 listener readiness
        EpollDriver poller(nullptr); // 删除 registration 后只允许忽略迟到事件
        Require(poller.Activate(), "EpollDriver should activate for StopAccept contracts");

        int acceptedCount = 0; // StopAccept 后必须保持零
        Require(poller.StartAccept(listener.Fd(), [&](int clientFd) {
            ++acceptedCount;
            (void)::close(clientFd);
        }), "Epoll accept should register before stop");
        const int clientFd = listener.ConnectClient(); // 内核已排队的迟到 readiness
        poller.StopAccept(listener.Fd());

        std::vector<Channel*> activeChannels; // 旧 registration id 已从映射撤销
        poller.Poll(30, activeChannels);
        Require(acceptedCount == 0, "StopAccept must suppress queued late events");
        (void)::close(clientFd);
    }

    // 验证 readiness Poller 激活后补注册 EventLoop 唤醒通道，连续投递不会退化到超时轮询。
    void TestEpollEventLoopPostTaskWakeup() {
        EventLoop loop(std::make_unique<EpollDriver>(nullptr)); // 强制隔离 epoll 激活时序
        loop.SetPollTimeout(500);
        std::atomic<bool> entered{ false }; // worker 已进入 Start
        std::atomic<int> completed{ 0 }; // 已执行的跨线程任务数量
        std::thread worker([&loop, &entered]() {
            entered.store(true, std::memory_order_release);
            loop.Start();
        });

        const auto startDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!entered.load(std::memory_order_acquire)
            && std::chrono::steady_clock::now() < startDeadline) {
            std::this_thread::yield();
        }
        Require(entered.load(std::memory_order_acquire), "Epoll EventLoop worker should start");
        std::this_thread::sleep_for(std::chrono::milliseconds(25)); // 确保 worker 已进入 epoll_wait

        for (int index = 0; index < 8; ++index) {
            std::atomic<bool> taskDone{ false }; // 当前任务只允许被执行一次
            loop.PostTask([&taskDone, &completed]() {
                taskDone.store(true, std::memory_order_release);
                completed.fetch_add(1, std::memory_order_release);
            });
            const auto taskDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
            while (!taskDone.load(std::memory_order_acquire)
                && std::chrono::steady_clock::now() < taskDeadline) {
                std::this_thread::yield();
            }
            Require(taskDone.load(std::memory_order_acquire),
                "Epoll EventLoop PostTask should wake before poll timeout");
        }

        loop.Shutdown();
        worker.join();
        Require(completed.load(std::memory_order_acquire) == 8,
            "Epoll EventLoop should execute every posted task exactly once");
    }
}

// 按 CTest 参数独立运行每个 RED/GREEN 原子契约。
int main(int argc, char** argv) {
    try {
        Require(argc == 2, "EpollDriver contract requires one case name");
        const std::string_view caseName(argv[1]); // 当前独立契约名称
        if (caseName == "channel") TestEpollChannelUpdateRemoveAndFdReuse();
        else if (caseName == "timeout-fire") TestEpollTimeoutFiresOnce();
        else if (caseName == "timeout-cancel") TestEpollTimeoutCancelSuppressesCallback();
        else if (caseName == "timeout-eintr") TestEpollTimeoutUsesSingleDeadlineAcrossEintr();
        else if (caseName == "connect") TestEpollConnectCompletesOnIssuerThread();
        else if (caseName == "connect-cancel") TestEpollConnectCancelClosesSocketWithoutCallback();
        else if (caseName == "connect-fd-reuse") TestEpollConnectFdReuseIgnoresLateRegistration();
        else if (caseName == "accept") TestEpollAcceptDrainsMultipleClients();
        else if (caseName == "accept-stop") TestEpollStopAcceptSuppressesLateEvents();
        else if (caseName == "event-loop-wakeup") TestEpollEventLoopPostTaskWakeup();
        else Require(false, "Unknown EpollDriver contract case");
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "EpollDriver contract failed: " << error.what() << '\n';
        return 1;
    }
}
