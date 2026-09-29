#include "net/DatagramCompletionPolicy.hpp"

#include <cstdlib>

namespace {
    // contract 失败立即终止，保持平台无关的最小运行入口。
    void Require(bool condition) {
        if (!condition) std::abort();
    }
}

int main() {
    using namespace LikesProgram::Net::Internal;

    Require(DatagramQueueCost(0) == 1);
    Require(DatagramQueueCost(7) == 7);

    const DatagramReceiveResult truncated = InterpretDatagramReceive(9, 4, true);
    Require(truncated.payloadBytes == 4);
    Require(truncated.originalBytes == 9);
    Require(truncated.truncated);

    const DatagramReceiveResult empty = InterpretDatagramReceive(0, 64, false);
    Require(empty.payloadBytes == 0);
    Require(empty.originalBytes == 0);
    Require(!empty.truncated);

    Require(DatagramWriteCompleted(0, 0));
    Require(DatagramWriteCompleted(7, 7));
    Require(!DatagramWriteCompleted(3, 7));
    Require(ResolveDatagramWriteTarget(true, true) == DatagramWriteTarget::ExplicitPeer);
    Require(ResolveDatagramWriteTarget(true, false) == DatagramWriteTarget::Connected);
    Require(ResolveDatagramWriteTarget(false, true) == DatagramWriteTarget::ExplicitPeer);
    Require(ResolveDatagramWriteTarget(false, false) == DatagramWriteTarget::Invalid);
    Require(ShouldResubmitDatagramRead(false, true));
    Require(!ShouldResubmitDatagramRead(true, true));
    Require(!ShouldResubmitDatagramRead(false, false));
    Require(!ShouldResubmitDatagramRead(false, true, true));
}
