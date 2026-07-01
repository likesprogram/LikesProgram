#include <LikesProgram/Metrics/Registry.hpp>

#include <list>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace LikesProgram {
    namespace Metrics {
        struct Registry::RegistryImpl {
            mutable std::shared_mutex m_mutex; // 保护指标列表和索引
            std::list<std::shared_ptr<Metric>> m_metricsList; // 保持注册顺序的指标列表
            std::unordered_map<std::string,
                std::list<std::shared_ptr<Metric>>::iterator> m_metricsMap; // name+labels 到列表迭代器索引
        };

        Registry& Registry::Global() {
            // C++11 起局部静态初始化线程安全，避免旧版手写单例泄漏。
            static Registry instance;
            return instance;
        }

        Registry::Registry()
            : m_impl(new RegistryImpl{}) {
        }

        Registry::~Registry() {
            delete m_impl;
            m_impl = nullptr;
        }

        void Registry::Register(const std::shared_ptr<Metric>& metric) {
            if (!metric || !m_impl) return;

            std::unique_lock lock(m_impl->m_mutex); // 注册会修改列表和索引
            const LikesProgram::String key = MakeKey(metric->Name(), metric->Labels());
            const std::string stableKey = key.ToStdString(); // unordered_map 使用 UTF-8 稳定键

            auto found = m_impl->m_metricsMap.find(stableKey);
            if (found != m_impl->m_metricsMap.end()) {
                m_impl->m_metricsList.erase(found->second);
                m_impl->m_metricsMap.erase(found);
            }

            auto iter = m_impl->m_metricsList.insert(m_impl->m_metricsList.end(), metric);
            m_impl->m_metricsMap.emplace(stableKey, iter);
        }

        void Registry::RegisterMetric(const std::shared_ptr<Metric>& metric) {
            // 兼容旧语义，内部仍走统一 Register。
            Register(metric);
        }

        void Registry::Unregister(const LikesProgram::String& name,
            const std::map<LikesProgram::String, LikesProgram::String>& labels) {
            if (!m_impl) return;

            std::unique_lock lock(m_impl->m_mutex); // 删除需要独占更新索引
            const std::string key = MakeKey(name, labels).ToStdString();
            auto found = m_impl->m_metricsMap.find(key);
            if (found == m_impl->m_metricsMap.end()) return;

            m_impl->m_metricsList.erase(found->second);
            m_impl->m_metricsMap.erase(found);
        }

        std::shared_ptr<Metric> Registry::GetMetrics(const LikesProgram::String& name,
            const std::map<LikesProgram::String, LikesProgram::String>& labels) {
            if (!m_impl) return nullptr;

            std::shared_lock lock(m_impl->m_mutex); // 查询只读，可与导出并发
            const std::string key = MakeKey(name, labels).ToStdString();
            auto found = m_impl->m_metricsMap.find(key);
            return found != m_impl->m_metricsMap.end() ? *(found->second) : nullptr;
        }

        LikesProgram::String Registry::ExportPrometheus() const {
            if (!m_impl) return {};

            std::vector<std::shared_ptr<Metric>> snapshot; // 指标对象快照，避免导出时长时间持锁
            {
                std::shared_lock lock(m_impl->m_mutex);
                snapshot.assign(m_impl->m_metricsList.begin(), m_impl->m_metricsList.end());
            }

            LikesProgram::String result;
            for (const auto& metric : snapshot) {
                if (metric) result.Append(metric->ToPrometheus()).Append(u"\n");
            }
            return result;
        }

        LikesProgram::String Registry::ExportJson() const {
            if (!m_impl) return u"[]";

            std::vector<std::shared_ptr<Metric>> snapshot; // 导出快照，避免锁内调用用户扩展指标
            {
                std::shared_lock lock(m_impl->m_mutex);
                snapshot.assign(m_impl->m_metricsList.begin(), m_impl->m_metricsList.end());
            }

            LikesProgram::String result = u"[";
            bool first = true; // JSON 数组逗号控制
            for (const auto& metric : snapshot) {
                if (!metric) continue;
                if (!first) result.Append(u",");
                result.Append(metric->ToJson());
                first = false;
            }
            result.Append(u"]");
            return result;
        }

        int64_t Registry::Count() const {
            if (!m_impl) return 0;

            std::shared_lock lock(m_impl->m_mutex); // 同时读取列表和索引大小
            if (m_impl->m_metricsList.size() != m_impl->m_metricsMap.size()) return -1;
            return static_cast<int64_t>(m_impl->m_metricsList.size());
        }

        void Registry::Clear() {
            if (!m_impl) return;

            std::unique_lock lock(m_impl->m_mutex); // 清空注册表需要独占
            m_impl->m_metricsList.clear();
            m_impl->m_metricsMap.clear();
        }

        LikesProgram::String Registry::MakeKey(const LikesProgram::String& name,
            const std::map<LikesProgram::String, LikesProgram::String>& labels) {
            LikesProgram::String result = name; // name 与标签块组合成稳定键
            result.Append(Metric::FormatLabels(labels));
            return result;
        }
    }
}
