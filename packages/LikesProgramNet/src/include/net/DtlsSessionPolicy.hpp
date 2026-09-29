#pragma once
#include <LikesProgram/Net/DtlsEngine.hpp>
#include <cstddef>
#include <cstdint>

namespace LikesProgram {
    namespace Net {
        namespace Internal {
            struct DtlsSessionLimits {
                std::size_t maximumSessions = 0;                         // Connection 可持有的会话总数
                std::size_t maximumPendingSessions = 0;                  // Handshaking 会话上限
                std::size_t maximumPendingCiphertextBytesPerSession = 0; // 单 peer 未回收密文成本上限
            };

            enum class DtlsResultValidation : std::uint8_t {
                Valid,
                ProtocolError,
                MessageTooLarge
            };

            // 判断总会话与 Handshaking 会话是否都还有容量。
            constexpr bool CanCreateDtlsSession(
                const DtlsSessionLimits& limits,
                std::size_t sessionCount,
                std::size_t pendingSessionCount) noexcept {
                return sessionCount < limits.maximumSessions
                    && pendingSessionCount < limits.maximumPendingSessions;
            }

            // 计算一个密文数据报占用的排队成本，零长度也至少消耗一个单位。
            constexpr std::size_t DtlsDatagramQueueCost(std::size_t datagramBytes) noexcept {
                return datagramBytes == 0 ? 1 : datagramBytes;
            }

            // 以减法比较避免加法溢出，判断当前 peer 是否还能排队密文。
            constexpr bool CanQueueDtlsCiphertext(
                const DtlsSessionLimits& limits,
                std::size_t pendingCiphertextCost,
                std::size_t datagramBytes) noexcept {
                const std::size_t maximum = limits.maximumPendingCiphertextBytesPerSession; // 当前 peer 上限
                if (pendingCiphertextCost > maximum) return false;
                return DtlsDatagramQueueCost(datagramBytes) <= maximum - pendingCiphertextCost;
            }

            // 仅允许当前 timer generation 的 completion 推进会话。
            constexpr bool DtlsTimerGenerationMatches(
                std::uint64_t expectedGeneration,
                std::uint64_t completedGeneration) noexcept {
                return expectedGeneration == completedGeneration;
            }

            // Active 会话只有发生明文、密文或状态推进时才刷新启用的 idle timer。
            constexpr bool ShouldRefreshDtlsIdle(
                bool idleTimeoutEnabled,
                DtlsState state,
                bool plaintextProgress,
                bool ciphertextProgress,
                bool stateProgress) noexcept {
                return idleTimeoutEnabled
                    && state == DtlsState::Active
                    && (plaintextProgress || ciphertextProgress || stateProgress);
            }

            // 校验 Engine 标量结果与输出摘要，保持策略层不依赖数据报容器。
            constexpr DtlsResultValidation ValidateDtlsResult(
                const DtlsResult& result,
                DtlsState state,
                bool inputConsumed,
                std::size_t ciphertextCount,
                std::size_t maximumCiphertextElementBytes,
                std::size_t plaintextCount,
                std::size_t maximumCiphertextDatagramBytes) noexcept {
                if (!inputConsumed) return DtlsResultValidation::ProtocolError;

                const bool armsTimer = HasDtlsAction(
                    result.actions, DtlsAction::ArmRetransmitTimer); // 本轮请求启动 timer
                const bool cancelsTimer = HasDtlsAction(
                    result.actions, DtlsAction::CancelRetransmitTimer); // 本轮请求取消 timer
                if (armsTimer && cancelsTimer) return DtlsResultValidation::ProtocolError;
                if (armsTimer != (result.retransmitAfterMilliseconds > 0)) {
                    return DtlsResultValidation::ProtocolError;
                }

                const bool reportsCiphertext = HasDtlsAction(
                    result.actions, DtlsAction::CiphertextReady); // Engine 声明的密文输出
                const bool hasCiphertext = ciphertextCount > 0; // Net 观察到的密文元素
                if (reportsCiphertext != hasCiphertext) return DtlsResultValidation::ProtocolError;
                if (reportsCiphertext && maximumCiphertextElementBytes == 0) {
                    return DtlsResultValidation::ProtocolError;
                }
                if (maximumCiphertextElementBytes > maximumCiphertextDatagramBytes) {
                    return DtlsResultValidation::MessageTooLarge;
                }

                const bool reportsPlaintext = HasDtlsAction(
                    result.actions, DtlsAction::PlaintextReady); // Engine 声明的明文输出
                const bool hasPlaintext = plaintextCount > 0; // 零长度明文仍以元素数量表达
                if (reportsPlaintext != hasPlaintext) return DtlsResultValidation::ProtocolError;
                if (reportsPlaintext && state != DtlsState::Active) {
                    return DtlsResultValidation::ProtocolError;
                }
                return DtlsResultValidation::Valid;
            }
        }
    }
}
