#include <LikesProgram/Quic/QuicAckDelay.hpp>

#include <cstdlib>
#include <limits>

namespace {
    void Require(bool condition) {
        if (!condition) std::abort();
    }
}

void RunQuicAckDelayTests() {
    using namespace LikesProgram::Quic;

    QuicAckDelayContext context;
    Require(DecodeQuicAckDelay(context, 1).error
        == QuicAckDelayError::NotConfigured);
    Require(ConfigureQuicAckDelay(context, 3, 80) == QuicAckDelayError::None);
    const auto decoded = DecodeQuicAckDelay(context, 10);
    Require(decoded.Succeeded() && decoded.delayMicroseconds == 80);
    Require(DecodeQuicAckDelay(context, 11).error
        == QuicAckDelayError::ExceedsMaximum);
    Require(ConfigureQuicAckDelay(context, 21, 80)
        == QuicAckDelayError::InvalidExponent);
    Require(context.configured && context.exponent == 3);

    Require(ConfigureQuicAckDelay(context, 20,
            std::numeric_limits<std::uint64_t>::max()) == QuicAckDelayError::None);
    Require(DecodeQuicAckDelay(context,
            (std::numeric_limits<std::uint64_t>::max() >> 20) + 1)
        .error == QuicAckDelayError::Overflow);
    context.Reset();
    Require(!context.configured && DecodeQuicAckDelay(context, 0).error
        == QuicAckDelayError::NotConfigured);
}
