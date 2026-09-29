#include <LikesProgram/Net/TlsEngineFactory.hpp>
#include <memory>
#include <mutex>
#include <utility>

namespace LikesProgram {
    namespace Net {
        struct TlsEngineFactory::TlsEngineFactoryImpl {
            struct FactoryState {
                CreateCallback m_create;                     // 每连接 Engine 创建回调
                SharedInitializer m_initializeShared;        // 证书上下文等共享资源初始化回调
                mutable std::once_flag m_sharedOnce;         // 复制工厂共享的一次性初始化闸门
                mutable bool m_sharedResult = true;          // 首次共享资源初始化结果
            };

            std::shared_ptr<FactoryState> m_state; // 复制工厂共享回调与初始化结果
        };

        TlsEngineFactory::TlsEngineFactory() = default;

        TlsEngineFactory::TlsEngineFactory(const TlsEngineFactory& other)
            : m_impl(other.m_impl ? new TlsEngineFactoryImpl(*other.m_impl) : nullptr) {
        }

        TlsEngineFactory::TlsEngineFactory(TlsEngineFactory&& other) noexcept
            : m_impl(other.m_impl) {
            other.m_impl = nullptr;
        }

        TlsEngineFactory::~TlsEngineFactory() {
            delete m_impl;
            m_impl = nullptr;
        }

        TlsEngineFactory& TlsEngineFactory::operator=(const TlsEngineFactory& other) {
            if (this == &other) return *this;

            auto* fresh = other.m_impl ? new TlsEngineFactoryImpl(*other.m_impl) : nullptr; // 先分配保持异常安全
            delete m_impl;
            m_impl = fresh;
            return *this;
        }

        TlsEngineFactory& TlsEngineFactory::operator=(TlsEngineFactory&& other) noexcept {
            if (this == &other) return *this;

            delete m_impl;
            m_impl = other.m_impl;
            other.m_impl = nullptr;
            return *this;
        }

        TlsEngineFactory::TlsEngineFactory(CreateCallback createCallback)
            : m_impl(new TlsEngineFactoryImpl{}) {
            m_impl->m_state = std::make_shared<TlsEngineFactoryImpl::FactoryState>();
            m_impl->m_state->m_create = std::move(createCallback);
        }

        TlsEngineFactory::TlsEngineFactory(
            CreateCallback createCallback,
            SharedInitializer sharedInitializer)
            : m_impl(new TlsEngineFactoryImpl{}) {
            m_impl->m_state = std::make_shared<TlsEngineFactoryImpl::FactoryState>();
            m_impl->m_state->m_create = std::move(createCallback);
            m_impl->m_state->m_initializeShared = std::move(sharedInitializer);
        }

        TlsEngineFactory::operator bool() const noexcept {
            return m_impl
                && m_impl->m_state
                && static_cast<bool>(m_impl->m_state->m_create);
        }

        std::unique_ptr<TlsEngine> TlsEngineFactory::Create() const noexcept {
            if (!m_impl || !m_impl->m_state || !m_impl->m_state->m_create) return {};

            try {
                return m_impl->m_state->m_create();
            }
            catch (...) {
                // 用户 Engine 创建失败不能越过异步连接建立边界。
                return {};
            }
        }

        bool TlsEngineFactory::InitializeSharedResources() const noexcept {
            if (!m_impl || !m_impl->m_state) return true;

            // 共享上下文只随工厂状态初始化一次，复制工厂不会重复加载证书。
            std::call_once(m_impl->m_state->m_sharedOnce, [state = m_impl->m_state]() {
                if (!state->m_initializeShared) return;

                try {
                    state->m_sharedResult = state->m_initializeShared();
                }
                catch (...) {
                    state->m_sharedResult = false;
                }
            });
            return m_impl->m_state->m_sharedResult;
        }
    }
}
