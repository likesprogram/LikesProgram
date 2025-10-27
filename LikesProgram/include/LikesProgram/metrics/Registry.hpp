#pragma once
#include "../system/LikesProgramLibExport.hpp"
#include "../String.hpp"
#include "Metrics.hpp"

namespace LikesProgram {
	namespace Metrics {
        class Registry {
        public:
            // 全局单例
            static Registry& Global();

            // 允许多实例（测试 / 多租户）
            Registry();
            ~Registry();

            // 禁止拷贝
            Registry(const Registry&) = delete;
            Registry& operator=(const Registry&) = delete;

            // 禁止移动（防止用户误操作破坏单例）
            Registry(Registry&&) = delete;
            Registry& operator=(Registry&&) = delete;

            void Register(const std::shared_ptr<MetricsObject>& m);

            void Unregister(const LikesProgram::String& name, const std::map<LikesProgram::String, LikesProgram::String>& labels = {});

            std::shared_ptr<MetricsObject> GetMetrics(const LikesProgram::String& name, const std::map<LikesProgram::String, LikesProgram::String>& labels = {});

            LikesProgram::String ExportPrometheus();
            LikesProgram::String ExportJson();

            int64_t Count() const;

        private:
            struct RegistryImpl;
            RegistryImpl* m_impl;

            // 生成唯一键：name + sorted(labels)
            static LikesProgram::String MakeKey(const LikesProgram::String& name, const std::map<LikesProgram::String, LikesProgram::String>& labels);
        };
	}
}