#include "net/platform/windows/IocpPoller.hpp"
#include "net/platform/windows/WindowsPollerBackendPolicy.hpp"

#include <cstdlib>
#include <memory>
#include <stdexcept>

namespace LikesProgram {
    namespace Net {
        std::unique_ptr<Poller> CreateDefaultPoller(EventLoop* ownerLoop) {
            char* ownedBackend = nullptr;
            const char* backend = nullptr;
            std::size_t backendLength = 0;
            if (_dupenv_s(&ownedBackend, &backendLength, "LIKESPROGRAM_NET_BACKEND") == 0) {
                backend = ownedBackend;
            }
            const Internal::WindowsPollerBackendRequest request =
                Internal::ParseWindowsPollerBackendRequest(backend);
            std::free(ownedBackend);
            if (request == Internal::WindowsPollerBackendRequest::UnsupportedKnown) {
                throw std::runtime_error(
                    "Requested Net backend is not supported on this platform");
            }
            return std::make_unique<Internal::IocpPoller>(ownerLoop);
        }
    }
}
