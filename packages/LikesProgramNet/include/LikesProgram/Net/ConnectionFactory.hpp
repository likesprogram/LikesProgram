#pragma once
#include <LikesProgram/Net/system/LikesProgramNetExport.hpp>
#include <LikesProgram/Net/SocketType.hpp>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace LikesProgram {
    namespace Net {
        class Connection;
        class EventLoop;

        class LIKESPROGRAM_NET_API ConnectionFactory {
        public:
            using CreateCallback = std::function<std::shared_ptr<Connection>(SocketType, EventLoop*)>;
            using SharedSecureInitializer = std::function<bool()>;

            // 创建空工厂，Server/Client 会回退到内置明文 transport。
            ConnectionFactory();
            // 复制工厂，共享一次性初始化状态。
            ConnectionFactory(const ConnectionFactory& other);
            // 移动工厂。
            ConnectionFactory(ConnectionFactory&& other) noexcept;
            // 释放工厂实现。
            ~ConnectionFactory();

            // 复制赋值工厂。
            ConnectionFactory& operator=(const ConnectionFactory& other);
            // 移动赋值工厂。
            ConnectionFactory& operator=(ConnectionFactory&& other) noexcept;

            // 直接使用 std::function 创建连接工厂。
            explicit ConnectionFactory(CreateCallback createCallback);
            // 直接使用 std::function 创建带共享初始化的连接工厂。
            ConnectionFactory(CreateCallback createCallback, SharedSecureInitializer sharedSecureInitializer);

            // 兼容旧式 lambda 工厂，只负责创建连接对象。
            template <
                typename CreateCallbackLike,
                typename = std::enable_if_t<!std::is_same_v<std::decay_t<CreateCallbackLike>, ConnectionFactory>>>
            ConnectionFactory(CreateCallbackLike&& createCallback)
                : ConnectionFactory(CreateCallback(std::forward<CreateCallbackLike>(createCallback))) {
            }

            // 创建带共享安全资源初始化的工厂，初始化回调由 Server/Client 启动期自动调用一次。
            template <
                typename CreateCallbackLike,
                typename SharedSecureInitializerLike,
                typename = std::enable_if_t<!std::is_same_v<std::decay_t<CreateCallbackLike>, ConnectionFactory>>>
            ConnectionFactory(
                CreateCallbackLike&& createCallback,
                SharedSecureInitializerLike&& sharedSecureInitializer)
                : ConnectionFactory(
                    CreateCallback(std::forward<CreateCallbackLike>(createCallback)),
                    WrapSharedSecureInitializer(std::forward<SharedSecureInitializerLike>(sharedSecureInitializer))) {
            }

            // 返回工厂是否持有用户自定义连接创建回调。
            explicit operator bool() const noexcept;

            // 创建一个连接；空工厂返回空指针，由调用方选择默认连接。
            std::shared_ptr<Connection> Create(SocketType fd, EventLoop* loop) const;

            // 初始化证书、SSL_CTX、ALPN、session cache 等跨连接共享安全资源。
            bool InitializeSharedSecureResources() const;

        private:
            struct ConnectionFactoryImpl;

            template <typename SharedSecureInitializerLike>
            static SharedSecureInitializer WrapSharedSecureInitializer(
                SharedSecureInitializerLike&& sharedSecureInitializer) {
                return [initializer = std::forward<SharedSecureInitializerLike>(sharedSecureInitializer)]() mutable {
                    using Result = std::invoke_result_t<SharedSecureInitializerLike&>;

                    if constexpr (std::is_void_v<Result>) {
                        initializer();
                        return true;
                    }
                    else {
                        return static_cast<bool>(initializer());
                    }
                };
            }

            ConnectionFactoryImpl* m_impl = nullptr; // 工厂实现，隐藏 std::function/once_flag 状态
        };
    }
}
