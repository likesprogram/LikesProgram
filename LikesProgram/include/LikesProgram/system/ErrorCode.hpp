#pragma once
#include "../String.hpp"
#include "LikesProgramLibExport.hpp"

namespace LikesProgram {
    namespace system {
        class LIKESPROGRAM_API ErrorCode {
        public:
            // 默认构造
            constexpr ErrorCode() = default;

            // 完整构造
            constexpr ErrorCode(int platform, int service, int module, int feature, int detail)
                : m_platform(platform), m_service(service), m_module(module),
                m_feature(feature), m_detail(detail) {
            }

            // 序列化为 int64
            constexpr int64_t ToInt64() const noexcept {
                return static_cast<int64_t>(m_platform) * 1000000000000LL
                    + static_cast<int64_t>(m_service) * 1000000000LL
                    + static_cast<int64_t>(m_module) * 1000000LL
                    + static_cast<int64_t>(m_feature) * 1000LL
                    + static_cast<int64_t>(m_detail);
            }

            // 从 int64 反序列化
            static constexpr ErrorCode FromInt64(int64_t code) noexcept {
                return ErrorCode(
                    static_cast<int>((code / 1000000000000LL) % 1000),
                    static_cast<int>((code / 1000000000LL) % 1000),
                    static_cast<int>((code / 1000000LL) % 1000),
                    static_cast<int>((code / 1000LL) % 1000),
                    static_cast<int>(code % 1000)
                );
            }

            // 工厂方法
            static constexpr ErrorCode Make(int platform, int service, int module, int feature, int detail) noexcept {
                return ErrorCode(platform, service, module, feature, detail);
            }

            // 分量访问
            constexpr int Platform() const noexcept { return m_platform; }
            constexpr int Service()  const noexcept { return m_service; }
            constexpr int Module()   const noexcept { return m_module; }
            constexpr int Feature()  const noexcept { return m_feature; }
            constexpr int Detail()   const noexcept { return m_detail; }

            // 是否为空
            constexpr bool IsNone() const noexcept { return ToInt64() == 0; }

            // 比较
            constexpr bool operator==(const ErrorCode& o) const noexcept { return ToInt64() == o.ToInt64(); }
            constexpr bool operator!=(const ErrorCode& o) const noexcept { return !(*this == o); }
            constexpr bool operator<(const ErrorCode& o) const noexcept { return ToInt64() < o.ToInt64(); }

            // 字符串化（仅数字结构，不带描述）
  /*          LikesProgram::String ToString() const {
                return LikesProgram::String::Format(u"%03d%03d%03d%03d%03d",
                    m_platform, m_service, m_module, m_feature, m_detail);
            }*/

        private:
            int m_platform = 0;
            int m_service = 0;
            int m_module = 0;
            int m_feature = 0;
            int m_detail = 0;
        };
    }
}