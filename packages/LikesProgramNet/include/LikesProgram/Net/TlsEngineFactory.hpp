#pragma once
#include <LikesProgram/Net/system/LikesProgramNetExport.hpp>
#include <LikesProgram/Net/TlsEngine.hpp>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace LikesProgram {
    namespace Net {
        class LIKESPROGRAM_NET_API TlsEngineFactory {
        public:
            using CreateCallback = std::function<std::unique_ptr<TlsEngine>()>;
            using SharedInitializer = std::function<bool()>;

            // 创建空工厂，Create 返回空 Engine。
            TlsEngineFactory();
            // 复制工厂时共享一次性资源初始化状态。
            TlsEngineFactory(const TlsEngineFactory& other);
            // 移动工厂并转移回调状态。
            TlsEngineFactory(TlsEngineFactory&& other) noexcept;
            // 释放回调与共享资源状态。
            ~TlsEngineFactory();

            // 复制赋值时共享源工厂状态。
            TlsEngineFactory& operator=(const TlsEngineFactory& other);
            // 移动赋值时转移源工厂状态。
            TlsEngineFactory& operator=(TlsEngineFactory&& other) noexcept;

            // 使用每连接 Engine 创建回调构造工厂。
            explicit TlsEngineFactory(CreateCallback createCallback);
            // 使用 Engine 创建回调和共享 TLS 资源初始化回调构造工厂。
            TlsEngineFactory(CreateCallback createCallback, SharedInitializer sharedInitializer);

            // 兼容 lambda 创建回调，用户只需实现 TlsEngine 派生类。
            template <
                typename CreateCallbackLike,
                typename = std::enable_if_t<!std::is_same_v<std::decay_t<CreateCallbackLike>, TlsEngineFactory>>>
            TlsEngineFactory(CreateCallbackLike&& createCallback)
                : TlsEngineFactory(CreateCallback(std::forward<CreateCallbackLike>(createCallback))) {
            }

            // 兼容 Engine 创建与共享资源初始化 lambda。
            template <
                typename CreateCallbackLike,
                typename SharedInitializerLike,
                typename = std::enable_if_t<!std::is_same_v<std::decay_t<CreateCallbackLike>, TlsEngineFactory>>>
            TlsEngineFactory(
                CreateCallbackLike&& createCallback,
                SharedInitializerLike&& sharedInitializer)
                : TlsEngineFactory(
                    CreateCallback(std::forward<CreateCallbackLike>(createCallback)),
                    WrapSharedInitializer(std::forward<SharedInitializerLike>(sharedInitializer))) {
            }

            // 返回工厂是否持有 Engine 创建回调。
            explicit operator bool() const noexcept;
            // 创建一个每连接 Engine；回调缺失或抛出异常时返回空指针。
            std::unique_ptr<TlsEngine> Create() const noexcept;
            // 初始化证书上下文、ALPN、session cache 等共享资源，复制工厂只执行一次。
            bool InitializeSharedResources() const noexcept;

        private:
            struct TlsEngineFactoryImpl;

            template <typename SharedInitializerLike>
            static SharedInitializer WrapSharedInitializer(SharedInitializerLike&& sharedInitializer) {
                return [initializer = std::forward<SharedInitializerLike>(sharedInitializer)]() mutable {
                    using Result = std::invoke_result_t<SharedInitializerLike&>;

                    if constexpr (std::is_void_v<Result>) {
                        initializer();
                        return true;
                    }
                    else {
                        return static_cast<bool>(initializer());
                    }
                };
            }

            TlsEngineFactoryImpl* m_impl = nullptr; // 隐藏回调、once_flag 与共享状态
        };
    }
}
