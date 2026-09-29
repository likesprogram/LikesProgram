#pragma once

#include <LikesProgram/Quic/system/LikesProgramQuicExport.hpp>
#include <LikesProgram/Core/Result.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace LikesProgram {
    namespace Quic {
        struct QuicCryptoFrame {
            std::uint64_t offset = 0;
            std::span<const std::uint8_t> data{};
            std::size_t consumedBytes = 0;
        };

        struct QuicCryptoFrameBuildOptions {
            std::uint64_t offset = 0;
            std::span<const std::uint8_t> data{};
        };

        // Parses one CRYPTO frame and returns a non-owning payload span.
        LIKESPROGRAM_QUIC_API Result<QuicCryptoFrame> ParseQuicCryptoFrame(
            const std::uint8_t* data,
            std::size_t size);

        // Builds one CRYPTO frame. TLS handshake state and packet protection
        // remain outside this wire-format helper.
        LIKESPROGRAM_QUIC_API Result<std::vector<std::uint8_t>>
            BuildQuicCryptoFrame(const QuicCryptoFrameBuildOptions& options);
    }
}
