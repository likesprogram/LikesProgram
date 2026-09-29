#include "net/platform/linux/EpollDriver.hpp"
#include "net/platform/linux/IoUringPoller.hpp"
#include "net/platform/linux/LinuxPollerBackendPolicy.hpp"

#include <cstdlib>
#include <memory>

namespace LikesProgram {
    namespace Net {
        std::unique_ptr<Poller> CreateDefaultPoller(EventLoop* ownerLoop) {
            const Internal::LinuxPollerBackendRequest request =
                Internal::ParseLinuxPollerBackendRequest(
                    std::getenv("LIKESPROGRAM_NET_BACKEND")); // 进程级覆盖只影响默认工厂
            if (request == Internal::LinuxPollerBackendRequest::Epoll) {
                return std::make_unique<Internal::EpollDriver>(ownerLoop);
            }

            auto ioUring = std::make_unique<Internal::IoUringPoller>(ownerLoop); // 保留已完成的构造期资源
            const Internal::LinuxPollerBackend backend = Internal::SelectLinuxPollerBackend(
                request,
                ioUring->IsReady()); // auto 只按构造期能力回退，不调用 Activate
            if (backend == Internal::LinuxPollerBackend::IoUring) return ioUring;
            return std::make_unique<Internal::EpollDriver>(ownerLoop);
        }
    }
}
