#pragma once

#include <LikesProgram/Net/IOEvent.hpp>
#include <LikesProgram/Net/SocketType.hpp>
#include <cstdint>
#include <vector>

namespace LikesProgram {
    namespace Net {
        namespace Internal {
            using ReadinessRegistrationId = std::uint64_t;

            struct ReadinessEvent {
                ReadinessRegistrationId registrationId = 0; // 逻辑 registration 身份
                IOEvent events = IOEvent::None; // 平台映射后的统一事件位
                int error = 0; // 平台等待器提供的错误码
            };

            // 判断统一 readiness 事件是否包含目标位。
            inline bool HasReadinessEvent(IOEvent events, IOEvent expected) noexcept {
                return (static_cast<std::uint32_t>(events)
                    & static_cast<std::uint32_t>(expected)) != 0;
            }

            class ReadinessDriver {
            public:
                virtual ~ReadinessDriver() = default;

                // 创建并启用平台等待器。
                virtual bool Activate() = 0;
                // 添加逻辑 registration 与关注事件。
                virtual bool Add(ReadinessRegistrationId id, SocketType fd, IOEvent events) = 0;
                // 更新逻辑 registration 的关注事件。
                virtual bool Modify(ReadinessRegistrationId id, SocketType fd, IOEvent events) = 0;
                // 删除逻辑 registration 并抑制迟到事件。
                virtual bool Remove(ReadinessRegistrationId id, SocketType fd) noexcept = 0;
                // 在相对 deadline 内等待并返回统一事件。
                virtual int Wait(int timeoutMs, std::vector<ReadinessEvent>& events) = 0;
                // 返回最近一次平台等待器错误。
                virtual int LastError() const noexcept = 0;
                // 返回私有稳定诊断名称。
                virtual const char* BackendName() const noexcept = 0;
            };
        }
    }
}
