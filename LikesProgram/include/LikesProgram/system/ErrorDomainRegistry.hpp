#pragma once
#include "LikesProgramLibExport.hpp"
#include "../String.hpp"

namespace LikesProgram {
	namespace System {
        class ErrorDomainRegistry {
        public:
            static ErrorDomainRegistry& Instance();

            // 注册新错误域，返回唯一 ID
            int Register(const String& name);

            // 根据 ID 获取名称
            String GetName(int id) const;

            ~ErrorDomainRegistry();
        private:
            ErrorDomainRegistry();
            struct ErrorDomainRegistryImpl;
            ErrorDomainRegistryImpl* m_impl;
        };
	}
}