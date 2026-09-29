#pragma once
#include <LikesProgram/Quic/system/LikesProgramQuicExport.hpp>
#include <LikesProgram/Quic/QuicEngine.hpp>
#include <functional>
#include <memory>

namespace LikesProgram {
    namespace Quic {
        class LIKESPROGRAM_QUIC_API QuicEngineFactory {
        public:
            using CreateCallback = std::function<std::unique_ptr<QuicEngine>(
                const QuicEngineOptions& options)>;
            using SharedInitializer = std::function<bool()>;

            // 创建空 Factory；Create 安全返回空 Engine。
            QuicEngineFactory();
            // 复制 Factory 时共享回调与一次性初始化状态。
            QuicEngineFactory(const QuicEngineFactory& other);
            // 移动 Factory 并转移回调状态。
            QuicEngineFactory(QuicEngineFactory&& other) noexcept;
            // 释放回调与共享资源状态。
            ~QuicEngineFactory();

            // 复制赋值时共享源 Factory 状态。
            QuicEngineFactory& operator=(const QuicEngineFactory& other);
            // 移动赋值时转移源 Factory 状态。
            QuicEngineFactory& operator=(QuicEngineFactory&& other) noexcept;

            // 使用 QUIC Engine 创建回调构造 Factory。
            explicit QuicEngineFactory(CreateCallback createCallback);
            // 使用 Engine 回调和共享资源初始化回调构造 Factory。
            QuicEngineFactory(
                CreateCallback createCallback,
                SharedInitializer sharedInitializer);

            // 返回 Factory 是否持有 Engine 创建回调。
            explicit operator bool() const noexcept;
            // 为指定选项创建独立 Engine；回调缺失或抛出时返回空指针。
            std::unique_ptr<QuicEngine> Create(
                const QuicEngineOptions& options) const noexcept;
            // 初始化用户提供的 TLS/QUIC 共享资源，复制对象只执行一次。
            bool InitializeSharedResources() const noexcept;

        private:
            struct QuicEngineFactoryImpl;

            QuicEngineFactoryImpl* m_impl = nullptr; // 隐藏回调、once_flag 与共享状态
        };
    }
}
