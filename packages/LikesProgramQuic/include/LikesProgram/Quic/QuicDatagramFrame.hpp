#pragma once

#include <LikesProgram/Quic/system/LikesProgramQuicExport.hpp>
#include <LikesProgram/Core/Result.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace LikesProgram {
    namespace Quic {
        struct QuicDatagramFrame {
            bool hasLength = true;
            std::span<const std::uint8_t> data{};
            std::size_t consumedBytes = 0;
        };

        LIKESPROGRAM_QUIC_API Result<QuicDatagramFrame>
            ParseQuicDatagramFrame(const std::uint8_t* data, std::size_t size);

        // hasLength=false emits DATAGRAM without a length field and consumes
        // the rest of the enclosing packet on parse.
        LIKESPROGRAM_QUIC_API Result<std::vector<std::uint8_t>>
            BuildQuicDatagramFrame(bool hasLength,
                std::span<const std::uint8_t> payload);
    }
}
