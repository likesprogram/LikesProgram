#include <LikesProgram/Quic/QuicEngineFactory.hpp>
#include <memory>
#include <mutex>
#include <string_view>
#include <utility>

namespace LikesProgram {
    namespace Quic {
        bool IsValidQuicEngineOptions(const QuicEngineOptions& options) noexcept {
            if (options.role != QuicRole::Client && options.role != QuicRole::Server) return false;
            if (options.tlsVersion != QuicTlsVersion::Tls13) return false;
            if (options.applicationProtocol == nullptr
                || std::string_view(options.applicationProtocol).empty()) return false;
            if (options.maximumDatagramBytes < 1200) return false;
            if (options.idleTimeout.count() < 0) return false;
            if (options.destinationConnectionIdLength > 20) return false;
            if (options.destinationConnectionId.size()
                != options.destinationConnectionIdLength) return false;
            return true;
        }

        struct QuicEngineFactory::QuicEngineFactoryImpl {
            struct FactoryState {
                CreateCallback m_createCallback; // 每连接 Engine 创建入口
                SharedInitializer m_initializer; // 用户 TLS/QUIC 共享初始化入口
                mutable std::once_flag m_initializeOnce; // 复制 Factory 共享的一次性闸门
                mutable bool m_initializeResult = true; // 首次初始化结果快照
            };

            std::shared_ptr<FactoryState> m_state; // 复制对象共享回调与初始化状态
        };

        QuicEngineFactory::QuicEngineFactory() = default;

        QuicEngineFactory::QuicEngineFactory(const QuicEngineFactory& other)
            : m_impl(other.m_impl ? new QuicEngineFactoryImpl(*other.m_impl) : nullptr) {
        }

        QuicEngineFactory::QuicEngineFactory(QuicEngineFactory&& other) noexcept
            : m_impl(other.m_impl) {
            other.m_impl = nullptr;
        }

        QuicEngineFactory::~QuicEngineFactory() {
            delete m_impl;
            m_impl = nullptr;
        }

        QuicEngineFactory& QuicEngineFactory::operator=(const QuicEngineFactory& other) {
            if (this == &other) return *this;

            // 先复制新 PImpl，分配失败时保持当前 Factory 不变。
            auto* fresh = other.m_impl
                ? new QuicEngineFactoryImpl(*other.m_impl)
                : nullptr; // 新共享状态外壳
            delete m_impl;
            m_impl = fresh;
            return *this;
        }

        QuicEngineFactory& QuicEngineFactory::operator=(QuicEngineFactory&& other) noexcept {
            if (this == &other) return *this;

            // 释放当前外壳后唯一接管源 Factory。
            delete m_impl;
            m_impl = other.m_impl;
            other.m_impl = nullptr;
            return *this;
        }

        QuicEngineFactory::QuicEngineFactory(CreateCallback createCallback) {
            auto state = std::make_shared<QuicEngineFactoryImpl::FactoryState>(); // 完整构造后再发布状态
            state->m_createCallback = std::move(createCallback);
            m_impl = new QuicEngineFactoryImpl{ std::move(state) };
        }

        QuicEngineFactory::QuicEngineFactory(
            CreateCallback createCallback,
            SharedInitializer sharedInitializer) {
            auto state = std::make_shared<QuicEngineFactoryImpl::FactoryState>(); // 回调全部就绪后发布状态
            state->m_createCallback = std::move(createCallback);
            state->m_initializer = std::move(sharedInitializer);
            m_impl = new QuicEngineFactoryImpl{ std::move(state) };
        }

        QuicEngineFactory::operator bool() const noexcept {
            return m_impl != nullptr
                && m_impl->m_state != nullptr
                && static_cast<bool>(m_impl->m_state->m_createCallback);
        }

        std::unique_ptr<QuicEngine> QuicEngineFactory::Create(
            const QuicEngineOptions& options) const noexcept {
            if (!IsValidQuicEngineOptions(options)
                || !m_impl || !m_impl->m_state || !m_impl->m_state->m_createCallback) return {};

            try {
                return m_impl->m_state->m_createCallback(options);
            }
            catch (...) {
                // 用户 Engine 创建异常不能越过异步连接创建边界。
                return {};
            }
        }

        bool QuicEngineFactory::InitializeSharedResources() const noexcept {
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
