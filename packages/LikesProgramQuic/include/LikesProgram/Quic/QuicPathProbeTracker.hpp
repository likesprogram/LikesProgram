#pragma once

#include <LikesProgram/Quic/system/LikesProgramQuicExport.hpp>

#include <array>
#include <cstdint>

namespace LikesProgram {
    namespace Quic {
        enum class QuicPathProbeError {
            None,
            ResponseWithoutChallenge,
            TokenMismatch,
        };

        struct QuicPathProbeState {
            bool pending = false;
            bool validated = false;
            std::array<std::uint8_t, 8> token{};

            void Reset() noexcept {
                pending = false;
                validated = false;
                token = {};
            }
        };

        struct QuicPathProbeResult {
            QuicPathProbeError error = QuicPathProbeError::None;
            bool advanced = false;

            bool Succeeded() const noexcept {
                return error == QuicPathProbeError::None;
            }
        };

        LIKESPROGRAM_QUIC_API QuicPathProbeResult ObserveQuicPathChallenge(
            QuicPathProbeState& state,
            const std::array<std::uint8_t, 8>& token) noexcept;

        LIKESPROGRAM_QUIC_API QuicPathProbeResult ObserveQuicPathResponse(
            QuicPathProbeState& state,
            const std::array<std::uint8_t, 8>& token) noexcept;
    }
}
