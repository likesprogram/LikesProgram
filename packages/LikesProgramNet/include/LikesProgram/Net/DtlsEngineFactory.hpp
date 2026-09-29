#pragma once
#include <LikesProgram/Net/system/LikesProgramNetExport.hpp>
#include <LikesProgram/Net/Address.hpp>
#include <LikesProgram/Net/DtlsEngine.hpp>
#include <cstddef>
#include <functional>
#include <memory>

namespace LikesProgram {
    namespace Net {
        class LIKESPROGRAM_NET_API DtlsEngineFactory {
        public:
            using CreateCallback = std::function<std::unique_ptr<DtlsEngine>(
                const Address& peer,
                const Address& local,
                std::size_t maximumCiphertextDatagramBytes)>;
            using SharedInitializer = std::function<bool()>;

            // 创建空 client Factory，Create 安全返回空 Engine。
            DtlsEngineFactory();
            // 复制 Factory 时共享一次性资源初始化状态。
            DtlsEngineFactory(const DtlsEngineFactory& other);
            // 移动 Factory 并转移回调状态。
            DtlsEngineFactory(DtlsEngineFactory&& other) noexcept;
            // 释放回调与共享资源状态。
            ~DtlsEngineFactory();

            // 复制赋值时共享源 Factory 状态。
            DtlsEngineFactory& operator=(const DtlsEngineFactory& other);
            // 移动赋值时转移源 Factory 状态。
            DtlsEngineFactory& operator=(DtlsEngineFactory&& other) noexcept;

            // 使用固定角色和每 peer Engine 创建回调构造 Factory。
            DtlsEngineFactory(DtlsRole role, CreateCallback createCallback);
            // 使用固定角色、Engine 回调和共享资源初始化回调构造 Factory。
            DtlsEngineFactory(
                DtlsRole role,
                CreateCallback createCallback,
                SharedInitializer sharedInitializer);

            // 返回当前 Factory 固定的 client/server 角色。
            DtlsRole Role() const noexcept;
            // 返回 Factory 是否持有 Engine 创建回调。
            explicit operator bool() const noexcept;
            // 为指定 peer 创建独立 Engine；回调缺失或抛出时返回空指针。
            std::unique_ptr<DtlsEngine> Create(
                const Address& peer,
                const Address& local,
                std::size_t maximumCiphertextDatagramBytes) const noexcept;
            // 初始化 cookie secret、证书上下文等共享资源，复制对象只执行一次。
            bool InitializeSharedResources() const noexcept;

        private:
            struct DtlsEngineFactoryImpl;

            DtlsEngineFactoryImpl* m_impl = nullptr; // 隐藏角色、回调、once_flag 与共享状态
        };
    }
}
