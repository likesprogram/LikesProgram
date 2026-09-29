#include <LikesProgram/Net/DtlsEngineFactory.hpp>
#include <memory>
#include <mutex>
#include <utility>

namespace LikesProgram {
    namespace Net {
        struct DtlsEngineFactory::DtlsEngineFactoryImpl {
            struct FactoryState {
                CreateCallback m_createCallback;       // 每 peer Engine 创建入口
                SharedInitializer m_initializer;       // cookie secret/证书等共享初始化入口
                mutable std::once_flag m_initializeOnce; // 复制 Factory 共享的一次性闸门
                mutable bool m_initializeResult = true;  // 首次初始化结果快照
            };

            DtlsRole m_role = DtlsRole::Client;      // 当前 Factory 固定角色
            std::shared_ptr<FactoryState> m_state;   // 复制对象共享回调与初始化状态
        };

        DtlsEngineFactory::DtlsEngineFactory() = default;

        DtlsEngineFactory::DtlsEngineFactory(const DtlsEngineFactory& other)
            : m_impl(other.m_impl ? new DtlsEngineFactoryImpl(*other.m_impl) : nullptr) {
        }

        DtlsEngineFactory::DtlsEngineFactory(DtlsEngineFactory&& other) noexcept
            : m_impl(other.m_impl) {
            other.m_impl = nullptr;
        }

        DtlsEngineFactory::~DtlsEngineFactory() {
            delete m_impl;
            m_impl = nullptr;
        }

        DtlsEngineFactory& DtlsEngineFactory::operator=(const DtlsEngineFactory& other) {
            if (this == &other) return *this;

            // 先复制新 PImpl，分配失败时保持当前 Factory 不变。
            auto* fresh = other.m_impl ? new DtlsEngineFactoryImpl(*other.m_impl) : nullptr; // 新共享状态外壳
            delete m_impl;
            m_impl = fresh;
            return *this;
        }

        DtlsEngineFactory& DtlsEngineFactory::operator=(DtlsEngineFactory&& other) noexcept {
            if (this == &other) return *this;

            // 释放当前外壳后唯一接管源 Factory。
            delete m_impl;
            m_impl = other.m_impl;
            other.m_impl = nullptr;
            return *this;
        }

        DtlsEngineFactory::DtlsEngineFactory(DtlsRole role, CreateCallback createCallback) {
            auto state = std::make_shared<DtlsEngineFactoryImpl::FactoryState>(); // 完整构造后再发布的共享状态
            state->m_createCallback = std::move(createCallback);
            m_impl = new DtlsEngineFactoryImpl{ role, std::move(state) };
        }

        DtlsEngineFactory::DtlsEngineFactory(
            DtlsRole role,
            CreateCallback createCallback,
            SharedInitializer sharedInitializer) {
            auto state = std::make_shared<DtlsEngineFactoryImpl::FactoryState>(); // 回调全部就绪后再发布 PImpl
            state->m_createCallback = std::move(createCallback);
            state->m_initializer = std::move(sharedInitializer);
            m_impl = new DtlsEngineFactoryImpl{ role, std::move(state) };
        }

        DtlsRole DtlsEngineFactory::Role() const noexcept {
            return m_impl ? m_impl->m_role : DtlsRole::Client;
        }

        DtlsEngineFactory::operator bool() const noexcept {
            return m_impl != nullptr
                && m_impl->m_state != nullptr
                && static_cast<bool>(m_impl->m_state->m_createCallback);
        }

        std::unique_ptr<DtlsEngine> DtlsEngineFactory::Create(
            const Address& peer,
            const Address& local,
            std::size_t maximumCiphertextDatagramBytes) const noexcept {
            if (!m_impl || !m_impl->m_state || !m_impl->m_state->m_createCallback) return {};

            try {
                return m_impl->m_state->m_createCallback(
                    peer,
                    local,
                    maximumCiphertextDatagramBytes);
            }
            catch (...) {
                // 用户 Engine 创建异常不能越过异步会话创建边界。
                return {};
            }
        }

        bool DtlsEngineFactory::InitializeSharedResources() const noexcept {
            if (!m_impl || !m_impl->m_state) return true;

            // 复制 Factory 共享同一个 once_flag 与初始化结果。
            std::call_once(m_impl->m_state->m_initializeOnce, [state = m_impl->m_state]() {
                if (!state->m_initializer) return;

                try {
                    state->m_initializeResult = state->m_initializer();
                }
                catch (...) {
                    state->m_initializeResult = false;
                }
            });
            return m_impl->m_state->m_initializeResult;
        }
    }
}
