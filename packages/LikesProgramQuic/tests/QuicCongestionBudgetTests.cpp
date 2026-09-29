#include <LikesProgram/Quic/QuicCongestionBudget.hpp>

#include <stdexcept>

namespace {
    void Require(bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    }
}

void RunQuicCongestionBudgetTests() {
    using namespace LikesProgram::Quic;
    QuicCongestionBudget budget({ 8, 2 });
    Require(budget.Snapshot().availableBytes == 8, "budget should start at its window");
    Require(budget.Reserve(1, 5).IsOk(), "budget should reserve in-window bytes");
    Require(budget.Snapshot().inFlightBytes == 5
        && budget.Snapshot().availableBytes == 3,
        "budget should expose in-flight accounting");
    Require(!budget.Reserve(2, 4).IsOk(), "budget should reject over-window bytes");
    Require(budget.SetWindow(6).IsOk(), "budget should accept a window above in-flight bytes");
    Require(!budget.SetWindow(4).IsOk(), "budget should reject a window below in-flight bytes");
    Require(budget.Lose(1).IsOk() && budget.Snapshot().inFlightBytes == 0,
        "loss should release exactly one reservation");
    Require(!budget.Lose(1).IsOk(), "duplicate loss should be rejected");
    Require(budget.Reserve(2, 6).IsOk() && budget.Acknowledge(2).IsOk(),
        "acknowledgement should retire a reservation");
    budget.Reset();
    Require(budget.Snapshot().availableBytes == 8
        && budget.Snapshot().reservations == 0,
        "budget reset should clear reservations");
}
