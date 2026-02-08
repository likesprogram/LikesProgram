#include "../../../include/LikesProgram/metrics/Registry.hpp"
#include <mutex>
#include <shared_mutex>
#include <list>
#include <atomic>
#include <unordered_map>

namespace LikesProgram {
	namespace Metrics {
		struct Registry::RegistryImpl {
			mutable std::shared_mutex m_mutex;
			std::list<std::shared_ptr<MetricsObject>> m_metrics_list;
			std::unordered_map<LikesProgram::String, std::list<std::shared_ptr<MetricsObject>>::iterator> m_metrics_map;
		};

		Registry& Registry::Global() {
			static std::atomic<Registry*> instance = nullptr;
			static std::mutex mutex;

			// 检查实例是否需要重新创建
			Registry* inst = instance.load(std::memory_order_acquire);
			if (!inst) {
				std::lock_guard lock(mutex);
				inst = instance.load(std::memory_order_relaxed);
				if (!inst) {
					inst = new Registry(); // 构造函数为私有或受限时，这里也可以访问
					instance.store(inst, std::memory_order_release);
				}
			}

			return *inst;
		}

		Registry::Registry() :m_impl(new RegistryImpl{}) { }
		Registry::~Registry() {
			if (m_impl) {
				delete m_impl;
				m_impl = nullptr;
			}
		}

		void Registry::Register(const std::shared_ptr<MetricsObject>& m) {
			if (!m) return;
			std::unique_lock lock(m_impl->m_mutex); // 独占锁
			LikesProgram::String key = MakeKey(m->Name(), m->Labels());
			// 如果已有同 key 的对象，先移除
			auto it = m_impl->m_metrics_map.find(key);
			if (it != m_impl->m_metrics_map.end()) {
				m_impl->m_metrics_list.erase(it->second);
				m_impl->m_metrics_map.erase(it);
			}

			// 插入新对象
			auto iter = m_impl->m_metrics_list.insert(m_impl->m_metrics_list.end(), m);
			m_impl->m_metrics_map.emplace(key, iter);
		}

		void Registry::Unregister(const LikesProgram::String& name, const std::map<LikesProgram::String, LikesProgram::String>& labels) {
			std::unique_lock lock(m_impl->m_mutex);
            LikesProgram::String key = MakeKey(name, labels);
            auto it = m_impl->m_metrics_map.find(key);
            if (it != m_impl->m_metrics_map.end()) {
                m_impl->m_metrics_list.erase(it->second);
                m_impl->m_metrics_map.erase(it);
            }
		}

		std::shared_ptr<MetricsObject> Registry::GetMetrics(const LikesProgram::String& name, const std::map<LikesProgram::String, LikesProgram::String>& labels) {
			std::shared_lock lock(m_impl->m_mutex); // 共享锁
            LikesProgram::String key = MakeKey(name, labels);
			auto it = m_impl->m_metrics_map.find(key);
			return (it != m_impl->m_metrics_map.end()) ? *(it->second) : nullptr;
		}

		LikesProgram::String Registry::ExportPrometheus() {
			std::shared_lock lock(m_impl->m_mutex); // 共享锁
			String result;
			for (auto& m : m_impl->m_metrics_list) result.Append(m->ToPrometheus()).Append(u"\n");
			return result;
		}

		LikesProgram::String Registry::ExportJson() {
			std::shared_lock lock(m_impl->m_mutex); // 共享锁
			String result = u"[";
			bool first = true;
			for (auto& m : m_impl->m_metrics_list) {
				if (!first) result.Append(u",");
				result.Append(m->ToJson());
				first = false;
			}
			result.Append(u"]");
			return result;
		}

		int64_t Registry::Count() const {
            std::shared_lock lock(m_impl->m_mutex); // 共享锁
			if (m_impl->m_metrics_list.size() != m_impl->m_metrics_map.size()) return -1;
            return m_impl->m_metrics_list.size();
		}

		LikesProgram::String Registry::MakeKey(const LikesProgram::String& name, const std::map<LikesProgram::String, LikesProgram::String>& labels) {
			String result = name;
			result.Append(MetricsObject::FormatLabels(labels));
			return result;
		}
	}
}