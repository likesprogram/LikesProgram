#pragma once
#include <LikesProgram/Quic/system/LikesProgramQuicExport.hpp>
#include <LikesProgram/Quic/QuicAckFrame.hpp>
#include <LikesProgram/Quic/QuicAckApplication.hpp>
#include <LikesProgram/Quic/QuicAckDelay.hpp>
#include <LikesProgram/Quic/QuicAckTracker.hpp>
#include <LikesProgram/Quic/QuicAckRecoveryLedger.hpp>
#include <LikesProgram/Quic/QuicCryptoFrame.hpp>
#include <LikesProgram/Quic/QuicConnectionCloseFrame.hpp>
#include <LikesProgram/Quic/QuicConnectionCredit.hpp>
#include <LikesProgram/Quic/QuicCreditReservation.hpp>
#include <LikesProgram/Quic/QuicConnectionIdFrame.hpp>
#include <LikesProgram/Quic/QuicConnectionIdTracker.hpp>
#include <LikesProgram/Quic/QuicDatagramFrame.hpp>
#include <LikesProgram/Quic/QuicFlowControlFrame.hpp>
#include <LikesProgram/Quic/QuicHandshakeDoneFrame.hpp>
#include <LikesProgram/Quic/QuicPathValidationFrame.hpp>
#include <LikesProgram/Quic/QuicPathValidationAction.hpp>
#include <LikesProgram/Quic/QuicPathProbeTracker.hpp>
#include <LikesProgram/Quic/QuicPathMigrationLedger.hpp>
#include <LikesProgram/Quic/QuicTimeout.hpp>
#include <LikesProgram/Quic/QuicEngine.hpp>
#include <LikesProgram/Quic/QuicEngineFactory.hpp>
#include <LikesProgram/Quic/QuicWireEngine.hpp>
#include <LikesProgram/Quic/QuicLongHeader.hpp>
#include <LikesProgram/Quic/QuicNewTokenFrame.hpp>
#include <LikesProgram/Quic/QuicPacketNumber.hpp>
#include <LikesProgram/Quic/QuicPacketNumberSpace.hpp>
#include <LikesProgram/Quic/QuicShortHeader.hpp>
#include <LikesProgram/Quic/QuicRetransmissionQueue.hpp>
#include <LikesProgram/Quic/QuicCongestionBudget.hpp>
#include <LikesProgram/Quic/QuicCreditCongestionLedger.hpp>
#include <LikesProgram/Quic/QuicFrameType.hpp>
#include <LikesProgram/Quic/QuicPacketProtection.hpp>
#include <LikesProgram/Quic/QuicStreamTerminalTracker.hpp>
#include <LikesProgram/Quic/QuicStreamControlAction.hpp>
#include <LikesProgram/Quic/QuicStreamEventMapping.hpp>
#include <LikesProgram/Quic/QuicConnectionCloseEventMapping.hpp>
#include <LikesProgram/Quic/QuicPingFrame.hpp>
#include <LikesProgram/Quic/QuicStreamFrame.hpp>
#include <LikesProgram/Quic/QuicStreamControlFrame.hpp>
#include <LikesProgram/Quic/QuicStreamCredit.hpp>
#include <LikesProgram/Quic/QuicVarInt.hpp>

namespace LikesProgram {
    namespace Quic {
        // 返回 Quic 包名，用于测试、示例和诊断输出。
        LIKESPROGRAM_QUIC_API const char* PackageName() noexcept;

        // 返回 Quic 包当前跟随 LikesProgram 的统一版本号。
        LIKESPROGRAM_QUIC_API const char* PackageVersion() noexcept;

        // 表示 Quic 包目标已被成功链接到当前进程。
        LIKESPROGRAM_QUIC_API bool PackageAvailable() noexcept;
    }
}
