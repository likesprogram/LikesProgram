#pragma once

#include <LikesProgram/Http/system/LikesProgramHttpExport.hpp>
#include <LikesProgram/Http/Http3.hpp>
#include <LikesProgram/Http/Http3QuicAdapter.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace LikesProgram {
    namespace Http {
        struct Http3QuicRequestStreamBridgeSnapshot {
            Http3RequestStreamSnapshot stream{};
            bool peerReset = false;
            bool peerStopSending = false;
            std::uint64_t transportErrorCode = 0;
            bool hasBodySink = false;
            bool bodyBlocked = false;
            std::size_t pendingWireBytes = 0;
            // These values let the caller coordinate transport reads without
            // transferring receive-window ownership into the bridge.
            std::size_t pendingBodyBytes = 0;
            std::size_t bodyBufferedBytes = 0;
            std::size_t bodyWritableBytes = 0;
            std::size_t bodyCapacityBytes = 0;
            bool bodyRetryReady = false;
            std::uint64_t bodyPulledBytes = 0;
            std::uint64_t bodyPulledSinceAttach = 0;
            bool streamReceiveCreditConfigured = false;
            std::uint64_t streamReceiveCreditBaseLimit = 0;
            std::uint64_t streamReceiveCreditTargetLimit = 0;
            bool streamReceiveCreditTargetValid = false;
        };

        struct Http3ConnectionReceiveCreditCoordinatorSnapshot {
            bool configured = false;
            std::uint64_t initialLimit = 0;
            std::size_t registeredStreams = 0;
            std::uint64_t aggregatePulledBytes = 0;
            std::uint64_t targetLimit = 0;
            bool targetValid = false;
        };

        // Aggregates caller-supplied request bridge snapshots without owning
        // bridges, body sinks, adapters or transport I/O.
        class Http3ConnectionReceiveCreditCoordinator {
        public:
            LIKESPROGRAM_HTTP_API Http3ConnectionReceiveCreditCoordinator();
            LIKESPROGRAM_HTTP_API ~Http3ConnectionReceiveCreditCoordinator();
            LIKESPROGRAM_HTTP_API Http3ConnectionReceiveCreditCoordinator(Http3ConnectionReceiveCreditCoordinator&&) noexcept;
            LIKESPROGRAM_HTTP_API Http3ConnectionReceiveCreditCoordinator& operator=(Http3ConnectionReceiveCreditCoordinator&&) noexcept;
            Http3ConnectionReceiveCreditCoordinator(const Http3ConnectionReceiveCreditCoordinator&) = delete;
            Http3ConnectionReceiveCreditCoordinator& operator=(const Http3ConnectionReceiveCreditCoordinator&) = delete;

            LIKESPROGRAM_HTTP_API Result<void> Configure(std::uint64_t initialLimit);
            LIKESPROGRAM_HTTP_API Result<void> RegisterRequestStream(const Http3QuicRequestStreamBridgeSnapshot& snapshot);
            LIKESPROGRAM_HTTP_API Result<bool> ObserveRequestStream(const Http3QuicRequestStreamBridgeSnapshot& snapshot);
            LIKESPROGRAM_HTTP_API Result<void> UnregisterRequestStream(std::uint64_t streamId);
            LIKESPROGRAM_HTTP_API Result<bool> RefreshConnectionReceiveCredit(Http3QuicAdapter& adapter);
            LIKESPROGRAM_HTTP_API Http3ConnectionReceiveCreditCoordinatorSnapshot Snapshot() const noexcept;
            LIKESPROGRAM_HTTP_API void Reset() noexcept;

        private:
            struct Impl;
            std::unique_ptr<Impl> m_impl;
        };

        // Couples one generic QUIC event stream to the existing bounded H3
        // wire decoder. The bridge owns neither QUIC objects nor transport I/O.
        class Http3QuicRequestStreamBridge {
        public:
            LIKESPROGRAM_HTTP_API explicit Http3QuicRequestStreamBridge(std::uint64_t streamId, Http3StreamBodyLimits bodyLimits = {}, Http3RequestStreamWireLimits wireLimits = {});
            LIKESPROGRAM_HTTP_API ~Http3QuicRequestStreamBridge();
            LIKESPROGRAM_HTTP_API Http3QuicRequestStreamBridge(Http3QuicRequestStreamBridge&&) noexcept;
            LIKESPROGRAM_HTTP_API Http3QuicRequestStreamBridge& operator=(Http3QuicRequestStreamBridge&&) noexcept;
            Http3QuicRequestStreamBridge(const Http3QuicRequestStreamBridge&) = delete;
            Http3QuicRequestStreamBridge& operator=(const Http3QuicRequestStreamBridge&) = delete;

            LIKESPROGRAM_HTTP_API Result<void> Feed(const Http3QuicEvent& event);
            LIKESPROGRAM_HTTP_API Result<void> AttachBodySink(HttpBodySink* sink);
            LIKESPROGRAM_HTTP_API bool HasBodySink() const noexcept;
            LIKESPROGRAM_HTTP_API Result<void> RetryPendingBody();
            // The initial limit is caller-owned transport state. Refresh emits
            // only after successful application Pull calls advance the target.
            LIKESPROGRAM_HTTP_API Result<void> ConfigureStreamReceiveCredit(std::uint64_t initialLimit);
            LIKESPROGRAM_HTTP_API Result<bool> RefreshStreamReceiveCredit(Http3QuicAdapter& adapter);
            LIKESPROGRAM_HTTP_API Http3QuicRequestStreamBridgeSnapshot Snapshot() const noexcept;
            LIKESPROGRAM_HTTP_API Result<Http3RequestStreamQuicActions> FailureActions() const;
            LIKESPROGRAM_HTTP_API Status LastError() const;
            LIKESPROGRAM_HTTP_API void Reset() noexcept;

        private:
            struct Impl;
            std::unique_ptr<Impl> m_impl;
        };
    }
}
