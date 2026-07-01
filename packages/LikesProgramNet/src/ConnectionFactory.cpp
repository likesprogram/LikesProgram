#include <LikesProgram/Net/ConnectionFactory.hpp>
#include <memory>
#include <mutex>
#include <utility>

namespace LikesProgram {
    namespace Net {
        struct ConnectionFactory::ConnectionFactoryImpl {
            struct FactoryState {
                CreateCallback m_create;                                  // 每连接创建回调
                SharedSecureInitializer m_initializeSharedSecureResources; // 共享安全资源初始化回调
                mutable std::once_flag m_sharedSecureOnce;                // 保证共享初始化只执行一次
                mutable bool m_sharedSecureResult = true;                 // 记录首次共享初始化结果
            };

            std::shared_ptr<FactoryState> m_state; // 复制工厂时共享一次性初始化状态
        };

        ConnectionFactory::ConnectionFactory() = default;

        ConnectionFactory::ConnectionFactory(const ConnectionFactory& other)
            : m_impl(other.m_impl ? new ConnectionFactoryImpl(*other.m_impl) : nullptr) {
        }

        ConnectionFactory::ConnectionFactory(ConnectionFactory&& other) noexcept
            : m_impl(other.m_impl) {
            other.m_impl = nullptr;
        }

        ConnectionFactory::~ConnectionFactory() {
            delete m_impl;
            m_impl = nullptr;
        }

        ConnectionFactory& ConnectionFactory::operator=(const ConnectionFactory& other) {
            if (this == &other) return *this;

            auto* fresh = other.m_impl ? new ConnectionFactoryImpl(*other.m_impl) : nullptr; // 先分配，保持异常安全
            delete m_impl;
            m_impl = fresh;
            return *this;
        }

        ConnectionFactory& ConnectionFactory::operator=(ConnectionFactory&& other) noexcept {
            if (this == &other) return *this;

            delete m_impl;
            m_impl = other.m_impl;
            other.m_impl = nullptr;
            return *this;
        }

        ConnectionFactory::ConnectionFactory(CreateCallback createCallback)
            : m_impl(new ConnectionFactoryImpl{}) {
            m_impl->m_state = std::make_shared<ConnectionFactoryImpl::FactoryState>();
            m_impl->m_state->m_create = std::move(createCallback);
        }

        ConnectionFactory::ConnectionFactory(
            CreateCallback createCallback,
            SharedSecureInitializer sharedSecureInitializer)
            : m_impl(new ConnectionFactoryImpl{}) {
            m_impl->m_state = std::make_shared<ConnectionFactoryImpl::FactoryState>();
            m_impl->m_state->m_create = std::move(createCallback);
            m_impl->m_state->m_initializeSharedSecureResources = std::move(sharedSecureInitializer);
        }

        ConnectionFactory::operator bool() const noexcept {
            return m_impl
                && m_impl->m_state
                && static_cast<bool>(m_impl->m_state->m_create);
        }

        std::shared_ptr<Connection> ConnectionFactory::Create(SocketType fd, EventLoop* loop) const {
            if (!m_impl || !m_impl->m_state || !m_impl->m_state->m_create) return {};
            return m_impl->m_state->m_create(fd, loop);
        }

        bool ConnectionFactory::InitializeSharedSecureResources() const {
            if (!m_impl || !m_impl->m_state) return true;

            // 共享资源只随工厂状态初始化一次，避免每个连接重复加载证书链。
            std::call_once(m_impl->m_state->m_sharedSecureOnce, [state = m_impl->m_state]() {
                if (state->m_initializeSharedSecureResources) {
                    try {
                        state->m_sharedSecureResult = state->m_initializeSharedSecureResources();
                    }
                    catch (...) {
                        state->m_sharedSecureResult = false;
                    }
                }
            });

            return m_impl->m_state->m_sharedSecureResult;
        }
    }
}
