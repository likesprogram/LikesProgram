#pragma once
#include <LikesProgram/Metrics/Metric.hpp>

#include <cstdint>
#include <memory>

namespace LikesProgram {
    namespace Metrics {
        // 指标注册表，按 name+labels 维护一组可导出的指标对象。
        class LIKESPROGRAM_METRICS_API Registry {
        public:
            // 返回进程内全局注册表。
            static Registry& Global();

            // 创建独立注册表，适合测试或多租户场景。
            Registry();
            // 释放注册表内部索引。
            ~Registry();

            // 禁止拷贝，避免两个注册表共享迭代器状态。
            Registry(const Registry&) = delete;
            Registry& operator=(const Registry&) = delete;

            // 禁止移动，避免全局注册表或外部引用失效。
            Registry(Registry&&) = delete;
            Registry& operator=(Registry&&) = delete;

            // 注册指标；同名同标签对象会被新对象替换。
            void Register(const std::shared_ptr<Metric>& metric);
            // 兼容旧版名称的注册入口。
            void RegisterMetric(const std::shared_ptr<Metric>& metric);
            // 删除指定 name+labels 的指标。
            void Unregister(const LikesProgram::String& name,
                const std::map<LikesProgram::String, LikesProgram::String>& labels = {});
            // 查询指定 name+labels 的指标对象。
            std::shared_ptr<Metric> GetMetrics(const LikesProgram::String& name,
                const std::map<LikesProgram::String, LikesProgram::String>& labels = {});

            // 导出注册表全部指标为 Prometheus text exposition。
            LikesProgram::String ExportPrometheus() const;
            // 导出注册表全部指标为 JSON 数组。
            LikesProgram::String ExportJson() const;

            // 返回当前注册指标数量。
            int64_t Count() const;
            // 清空注册表，主要用于测试隔离。
            void Clear();
        private:
            struct RegistryImpl;
            RegistryImpl* m_impl = nullptr; // 指标列表、索引和读写锁实现

            // 生成稳定唯一键，labels 由 std::map 自然排序。
            static LikesProgram::String MakeKey(const LikesProgram::String& name,
                const std::map<LikesProgram::String, LikesProgram::String>& labels);
        };
    }
}
