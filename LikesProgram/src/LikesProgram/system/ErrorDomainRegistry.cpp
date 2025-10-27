#include "../../../include/LikesProgram/system/ErrorDomainRegistry.hpp"
#include <shared_mutex>
#include <mutex>
#include <unordered_map>

namespace LikesProgram {
	namespace System {
		struct ErrorDomainRegistry::ErrorDomainRegistryImpl {
			mutable std::shared_mutex mutex_;
			std::unordered_map<int, String> domains_;
			int nextId_ = 100; // 0~99 保留给框架内部
		};

		ErrorDomainRegistry& ErrorDomainRegistry::Instance() {
			static std::atomic<ErrorDomainRegistry*> instance{ nullptr };
			static std::mutex mutex;

			// 检查实例是否需要重新创建
			ErrorDomainRegistry* inst = instance.load(std::memory_order_acquire);
			if (!inst) {
				std::lock_guard lock(mutex);
				inst = instance.load(std::memory_order_relaxed);
				if (!inst) {
					inst = new ErrorDomainRegistry(); // 构造函数为私有或受限时，这里也可以访问
					instance.store(inst, std::memory_order_release);
				}
			}

			return *inst;
		}

		int ErrorDomainRegistry::Register(const String& name)
		{
			return 0;
		}

		String ErrorDomainRegistry::GetName(int id) const
		{
			return String();
		}

		ErrorDomainRegistry::ErrorDomainRegistry() : m_impl(new ErrorDomainRegistryImpl{}) {
			// 初始化框架内置错误域
			std::unique_lock lock(m_impl->mutex_);

			m_impl->domains_[0] = u"None";
			m_impl->domains_[1] = u"System";
			m_impl->domains_[2] = u"Threading";
			m_impl->domains_[3] = u"Config";
			m_impl->domains_[4] = u"Metrics";
			m_impl->domains_[5] = u"Logger";
			m_impl->domains_[6] = u"Reactor";

			m_impl->nextId_ = 100; // 用户自定义域从100开始分配
		}

		ErrorDomainRegistry::~ErrorDomainRegistry() {
            if (m_impl) {
                delete m_impl;
                m_impl = nullptr;
            }
		}
	}
}