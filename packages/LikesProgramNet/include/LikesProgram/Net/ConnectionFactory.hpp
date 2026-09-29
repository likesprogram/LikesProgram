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

            // 创建空工厂，Server/Client 会回退到内置协议连接。
            ConnectionFactory();
            // 复制工厂回调。
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

            // 兼容旧式 lambda 工厂，只负责创建连接对象。
            template <
                typename CreateCallbackLike,
                typename = std::enable_if_t<!std::is_same_v<std::decay_t<CreateCallbackLike>, ConnectionFactory>>>
            ConnectionFactory(CreateCallbackLike&& createCallback)
                : ConnectionFactory(CreateCallback(std::forward<CreateCallbackLike>(createCallback))) {
            }

            // 返回工厂是否持有用户自定义连接创建回调。
            explicit operator bool() const noexcept;

            // 创建一个连接；空工厂返回空指针，由调用方选择默认连接。
            std::shared_ptr<Connection> Create(SocketType fd, EventLoop* loop) const;

        private:
            struct ConnectionFactoryImpl;

            ConnectionFactoryImpl* m_impl = nullptr; // 工厂实现，隐藏 std::function 状态
        };
    }
}
