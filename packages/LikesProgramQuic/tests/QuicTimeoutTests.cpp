#include <LikesProgram/Quic/QuicTimeout.hpp>

#include <chrono>
#include <cstdlib>

namespace {
    void Require(bool condition) {
        if (!condition) std::abort();
    }
}

void RunQuicTimeoutTests() {
    using namespace LikesProgram::Quic;
    using namespace LikesProgram::Time;
    const auto base = SteadyTimePoint(std::chrono::seconds(100));

    QuicTimeoutContext context;
    Require(!EvaluateQuicTimeout(context, base).Expired());

    context.idle = Deadline::At(base + std::chrono::seconds(2));
    Require(!EvaluateQuicTimeout(context, base + std::chrono::seconds(1)).Expired());
    const auto idle = EvaluateQuicTimeout(context, base + std::chrono::seconds(2));
    Require(idle.reason == QuicTimeoutReason::IdleDeadline);
    Require(idle.action == QuicTimeoutAction::CloseIdle);

    context.handshake = Deadline::At(base + std::chrono::seconds(1));
    const auto handshake = EvaluateQuicTimeout(context, base + std::chrono::seconds(2));
    Require(handshake.reason == QuicTimeoutReason::HandshakeDeadline);
    Require(handshake.action == QuicTimeoutAction::FailHandshake);

    context.closing = Deadline::At(base + std::chrono::seconds(1));
    const auto closing = EvaluateQuicTimeout(context, base + std::chrono::seconds(2));
    Require(closing.reason == QuicTimeoutReason::ClosingDeadline);
    Require(closing.action == QuicTimeoutAction::FinishClosing);

    context.Reset();
    Require(!EvaluateQuicTimeout(context, base + std::chrono::seconds(3)).Expired());
}
