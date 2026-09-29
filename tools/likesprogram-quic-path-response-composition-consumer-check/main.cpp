#include <LikesProgram/Quic/QuicEngine.hpp>
#include <LikesProgram/Quic/QuicPathProbeTracker.hpp>
#include <LikesProgram/Quic/QuicPathValidationAction.hpp>
#include <LikesProgram/Quic/QuicPathValidationFrame.hpp>

#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
    using namespace LikesProgram::Quic;

    void Require(bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    }

    class DeliverySink final : public QuicActionSink {
    public:
        QuicActionDelivery Submit(QuicActionMessage&& action) noexcept override {
            if (action.kind != QuicActionKind::PathResponse
                || action.actionId != 0) {
                return { QuicActionDeliveryState::Rejected,
                    QuicActionDeliveryError::InvalidAction, 0 };
            }
            received.push_back(std::move(action));
            const auto id = nextId++;
            last = { nextState, nextError, id };
            nextState = QuicActionDeliveryState::Accepted;
            nextError = QuicActionDeliveryError::None;
            return last;
        }

        std::vector<QuicActionMessage> received;
        QuicActionDeliveryState nextState = QuicActionDeliveryState::Accepted;
        QuicActionDeliveryError nextError = QuicActionDeliveryError::None;
        QuicActionDelivery last{};
        std::uint64_t nextId = 1;
    };
}

int Run() {
    using namespace LikesProgram::Quic;
    const std::array<std::uint8_t, 8> token{ 0, 1, 2, 3, 4, 5, 6, 7 };
    const std::array<std::uint8_t, 8> wrongToken{ 7, 6, 5, 4, 3, 2, 1, 0 };
    const QuicPathValidationFrame challenge{
        QuicPathValidationFrameKind::PathChallenge, token, 0 };
    const auto challengeBytes = BuildQuicPathValidationFrame(challenge);
    Require(challengeBytes.IsOk() && challengeBytes.Value().size() == 9,
        "PATH_CHALLENGE should build to its fixed wire size");
    const auto parsedChallenge = ParseQuicPathValidationFrame(
        challengeBytes.Value().data(), challengeBytes.Value().size());
    Require(parsedChallenge.IsOk()
        && parsedChallenge.Value().kind == QuicPathValidationFrameKind::PathChallenge
        && parsedChallenge.Value().data == token
        && parsedChallenge.Value().consumedBytes == 9,
        "PATH_CHALLENGE should parse with its token and consumed boundary");

    QuicPathProbeState state;
    Require(ObserveQuicPathChallenge(state, parsedChallenge.Value().data).advanced
        && state.pending && !state.validated,
        "a parsed challenge should become the pending probe");
    Require(!ObserveQuicPathChallenge(state, token).advanced,
        "a duplicate challenge token should not advance probe generation");

    const auto actionResult = BuildQuicPathResponseAction(parsedChallenge.Value());
    Require(actionResult.IsOk()
        && actionResult.Value().kind == QuicActionKind::PathResponse
        && actionResult.Value().actionId == 0
        && actionResult.Value().payload.ReadableBytes() == 9,
        "a challenge should map to an unassigned PATH_RESPONSE action");
    const auto responsePayload = actionResult.Value().payload.AsStringView();
    const auto responseFrame = ParseQuicPathValidationFrame(
        reinterpret_cast<const std::uint8_t*>(responsePayload.data()),
        responsePayload.size());
    Require(responseFrame.IsOk()
        && responseFrame.Value().kind == QuicPathValidationFrameKind::PathResponse
        && responseFrame.Value().data == token,
        "the mapped action should carry the echoed PATH_RESPONSE bytes");

    DeliverySink sink;
    sink.nextState = QuicActionDeliveryState::Deferred;
    sink.nextError = QuicActionDeliveryError::Backpressure;
    auto deferredAction = actionResult.Value();
    const auto deferred = sink.Submit(std::move(deferredAction));
    Require(deferred.state == QuicActionDeliveryState::Deferred
        && deferred.error == QuicActionDeliveryError::Backpressure
        && deferred.actionId == 1
        && state.pending && !state.validated,
        "deferred delivery should retain the pending probe for caller retry");

    auto retryAction = actionResult.Value();
    const auto accepted = sink.Submit(std::move(retryAction));
    Require(accepted.Accepted() && accepted.actionId == 2
        && sink.received.size() == 2,
        "retry should deliver a fresh action id and owned payload");
    Require(ObserveQuicPathResponse(state, responseFrame.Value().data).advanced
        && !state.pending && state.validated,
        "caller acceptance followed by matching response should validate the probe");

    QuicPathProbeState rejectedState;
    Require(ObserveQuicPathChallenge(rejectedState, token).Succeeded(),
        "second probe should be caller-started");
    Require(ObserveQuicPathResponse(rejectedState, wrongToken).error
        == QuicPathProbeError::TokenMismatch
        && rejectedState.pending && !rejectedState.validated,
        "wrong response token should not validate the path");
    Require(!BuildQuicPathResponseAction({
            QuicPathValidationFrameKind::PathResponse, token, 0 }).IsOk(),
        "PATH_RESPONSE input should not be remapped as a challenge");
    rejectedState.Reset();
    Require(!rejectedState.pending && !rejectedState.validated,
        "probe reset should clear pending and validated state");

    std::cout << "passed=true"
              << " challenge_bytes=9"
              << " response_action=true"
              << " deferred_retry_ids=true"
              << " validated_after_accept=true"
              << " mismatch_preserves=true"
              << " reset=true\n";
    return 0;
}

int main() {
    try {
        return Run();
    }
    catch (const std::exception& error) {
        std::cerr << "failed=" << error.what() << '\n';
        return 99;
    }
}
