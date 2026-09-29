#pragma once

#include <LikesProgram/Core/Status.hpp>

#include <cstddef>
#include <cstdint>

namespace LikesProgram {
    namespace Http {
        enum class HttpVersion : std::uint8_t {
            Http1,
            Http2,
            Http3
        };

        enum class HttpErrorScope : std::uint8_t {
            None,
            Connection,
            Stream
        };

        enum class HttpErrorOrigin : std::uint8_t {
            Protocol,
            Application,
            Resource,
            Lifecycle,
            Transport
        };

        enum class HttpErrorUnitKind : std::uint8_t {
            None,
            Frame,
            Event
        };

        // Stable coordinates for correlating a Status with the owning
        // connection object, a stream, and an optional protocol wire unit.
        struct HttpErrorContext {
            bool valid = false;
            HttpVersion version = HttpVersion::Http1;
            HttpErrorScope scope = HttpErrorScope::None;
            HttpErrorOrigin origin = HttpErrorOrigin::Protocol;
            std::uint64_t streamId = 0;
            HttpErrorUnitKind unitKind = HttpErrorUnitKind::None;
            std::uint64_t unitType = 0;
            std::size_t byteOffset = 0;
            StatusCode statusCode = StatusCode::Ok;
        };
    }
}
