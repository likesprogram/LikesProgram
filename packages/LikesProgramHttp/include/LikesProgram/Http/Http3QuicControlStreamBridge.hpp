#pragma once

#include <LikesProgram/Http/system/LikesProgramHttpExport.hpp>
#include <LikesProgram/Http/Http3Control.hpp>
#include <LikesProgram/Http/Http3QuicAdapter.hpp>

#include <cstdint>
#include <memory>

namespace LikesProgram {
    namespace Http {
        struct Http3QuicControlStreamBridgeSnapshot {
            Http3ControlStreamSnapshot control{};
            bool peerReset = false;
            bool peerStopSending = false;
            std::uint64_t transportErrorCode = 0;
        };

        // Couples one generic QUIC event stream to the H3 control wire
        // decoder. The bridge does not emit CONNECTION_CLOSE or own QUIC.
        class Http3QuicControlStreamBridge {
        public:
            LIKESPROGRAM_HTTP_API explicit Http3QuicControlStreamBridge(std::size_t maxSettings = 64, Http3ControlStreamWireLimits wireLimits = {});
            LIKESPROGRAM_HTTP_API ~Http3QuicControlStreamBridge();
            LIKESPROGRAM_HTTP_API Http3QuicControlStreamBridge(Http3QuicControlStreamBridge&&) noexcept;
            LIKESPROGRAM_HTTP_API Http3QuicControlStreamBridge& operator=(Http3QuicControlStreamBridge&&) noexcept;
            Http3QuicControlStreamBridge(const Http3QuicControlStreamBridge&) = delete;
            Http3QuicControlStreamBridge& operator=(const Http3QuicControlStreamBridge&) = delete;

            LIKESPROGRAM_HTTP_API Result<void> Feed(const Http3QuicEvent& event);
            LIKESPROGRAM_HTTP_API Http3QuicControlStreamBridgeSnapshot Snapshot() const noexcept;
            LIKESPROGRAM_HTTP_API Result<Http3ControlStreamQuicActions> FailureActions() const;
            LIKESPROGRAM_HTTP_API Status LastError() const;
            LIKESPROGRAM_HTTP_API void Reset() noexcept;

        private:
            struct Impl;
            std::unique_ptr<Impl> m_impl;
        };
    }
}
