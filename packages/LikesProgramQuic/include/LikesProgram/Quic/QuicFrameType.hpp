#pragma once

#include <LikesProgram/Quic/system/LikesProgramQuicExport.hpp>
#include <LikesProgram/Quic/QuicVarInt.hpp>

#include <cstddef>
#include <cstdint>

namespace LikesProgram {
    namespace Quic {
        enum class QuicFrameType : std::uint8_t {
            Unknown,
            Padding,
            Ping,
            Ack,
            ResetStream,
            StopSending,
            Crypto,
            NewToken,
            Stream,
            MaxData,
            MaxStreamData,
            MaxStreams,
            DataBlocked,
            StreamDataBlocked,
            StreamsBlocked,
            NewConnectionId,
            RetireConnectionId,
            PathChallenge,
            PathResponse,
            ConnectionClose,
            HandshakeDone,
            Datagram
        };

        struct QuicFrameTypeView {
            std::uint64_t value = 0;
            std::size_t encodedBytes = 0;
            QuicFrameType kind = QuicFrameType::Unknown;
        };

        LIKESPROGRAM_QUIC_API QuicFrameType ClassifyQuicFrameType(
            std::uint64_t value) noexcept;

        // Parses only the frame-type varint; payload parsing and frame
        // dispatch remain with the caller-owned packet adapter.
        LIKESPROGRAM_QUIC_API Result<QuicFrameTypeView> ParseQuicFrameType(
            const std::uint8_t* data,
            std::size_t size);
    }
}
