#include "net/DtlsSessionPolicy.hpp"
#include <cstdlib>

namespace {
    // 失败时立即终止，保持纯策略契约不依赖测试框架。
    void Require(bool condition) {
        if (!condition) std::abort();
    }
}

// 验证资源上限、timer generation、idle 刷新与 Engine 结果约束。
int main() {
    using LikesProgram::Net::DtlsAction;
    using LikesProgram::Net::DtlsResult;
    using LikesProgram::Net::DtlsState;
    using LikesProgram::Net::Internal::CanCreateDtlsSession;
    using LikesProgram::Net::Internal::CanQueueDtlsCiphertext;
    using LikesProgram::Net::Internal::DtlsDatagramQueueCost;
    using LikesProgram::Net::Internal::DtlsResultValidation;
    using LikesProgram::Net::Internal::DtlsSessionLimits;
    using LikesProgram::Net::Internal::DtlsTimerGenerationMatches;
    using LikesProgram::Net::Internal::ShouldRefreshDtlsIdle;
    using LikesProgram::Net::Internal::ValidateDtlsResult;

    constexpr DtlsSessionLimits limits{ 1024, 256, 256 * 1024 }; // 默认资源保护基线
    static_assert(CanCreateDtlsSession(limits, 0, 0));
    static_assert(!CanCreateDtlsSession(limits, 1024, 0));
    static_assert(!CanCreateDtlsSession(limits, 100, 256));
    static_assert(DtlsDatagramQueueCost(0) == 1);
    static_assert(DtlsDatagramQueueCost(1200) == 1200);
    static_assert(CanQueueDtlsCiphertext(limits, 0, 256 * 1024));
    static_assert(!CanQueueDtlsCiphertext(limits, 256 * 1024, 1));
    static_assert(DtlsTimerGenerationMatches(7, 7));
    static_assert(!DtlsTimerGenerationMatches(7, 8));

    Require(ShouldRefreshDtlsIdle(true, DtlsState::Active, true, false, false));
    Require(ShouldRefreshDtlsIdle(true, DtlsState::Active, false, true, false));
    Require(ShouldRefreshDtlsIdle(true, DtlsState::Active, false, false, true));
    Require(!ShouldRefreshDtlsIdle(true, DtlsState::Active, false, false, false));
    Require(!ShouldRefreshDtlsIdle(false, DtlsState::Active, true, false, false));
    Require(!ShouldRefreshDtlsIdle(true, DtlsState::Handshaking, true, false, false));

    constexpr auto valid = DtlsResultValidation::Valid;
    constexpr auto protocolError = DtlsResultValidation::ProtocolError;
    constexpr auto messageTooLarge = DtlsResultValidation::MessageTooLarge;
    static_assert(ValidateDtlsResult(
        DtlsResult{}, DtlsState::Active, true, 0, 0, 0, 1200) == valid);
    const DtlsResult noActions{}; // 无输出、无 timer 的成功结果
    Require(ValidateDtlsResult(noActions, DtlsState::Active, true, 0, 0, 0, 1200) == valid);
    Require(ValidateDtlsResult(noActions, DtlsState::Active, false, 0, 0, 0, 1200) == protocolError);

    const DtlsResult conflictingTimers{
        DtlsAction::ArmRetransmitTimer | DtlsAction::CancelRetransmitTimer,
        0,
        25
    }; // timer 动作互斥
    Require(ValidateDtlsResult(conflictingTimers, DtlsState::Active, true, 0, 0, 0, 1200)
        == protocolError);
    const DtlsResult nonPositiveArm{ DtlsAction::ArmRetransmitTimer, 0, 0 }; // arm 必须给正延迟
    Require(ValidateDtlsResult(nonPositiveArm, DtlsState::Active, true, 0, 0, 0, 1200)
        == protocolError);

    const DtlsResult ciphertextReady{ DtlsAction::CiphertextReady, 0, 0 }; // 声明产生密文
    Require(ValidateDtlsResult(ciphertextReady, DtlsState::Active, true, 0, 0, 0, 1200)
        == protocolError);
    Require(ValidateDtlsResult(ciphertextReady, DtlsState::Active, true, 1, 0, 0, 1200)
        == protocolError);
    Require(ValidateDtlsResult(ciphertextReady, DtlsState::Active, true, 1, 1200, 0, 1200)
        == valid);
    Require(ValidateDtlsResult(ciphertextReady, DtlsState::Active, true, 1, 1201, 0, 1200)
        == messageTooLarge);
    Require(ValidateDtlsResult(noActions, DtlsState::Active, true, 1, 1, 0, 1200)
        == protocolError);

    const DtlsResult plaintextReady{ DtlsAction::PlaintextReady, 0, 0 }; // 零长度明文元素仍合法
    Require(ValidateDtlsResult(plaintextReady, DtlsState::Active, true, 0, 0, 0, 1200)
        == protocolError);
    Require(ValidateDtlsResult(plaintextReady, DtlsState::Handshaking, true, 0, 0, 1, 1200)
        == protocolError);
    Require(ValidateDtlsResult(plaintextReady, DtlsState::Active, true, 0, 0, 1, 1200)
        == valid);
    Require(ValidateDtlsResult(noActions, DtlsState::Active, true, 0, 0, 1, 1200)
        == protocolError);
}
