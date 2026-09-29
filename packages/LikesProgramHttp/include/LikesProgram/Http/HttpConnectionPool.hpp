#pragma once

#include <LikesProgram/Http/system/LikesProgramHttpExport.hpp>
#include <LikesProgram/Http/HttpSession.hpp>
#include <LikesProgram/Core/Result.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace LikesProgram {
    namespace Http {
        enum class HttpConnectionState : std::uint8_t {
            Ready,
            Draining,
            Migrating,
            Closed
        };

        struct HttpConnectionPoolLimits {
            std::size_t maxConnections = 32;
            std::size_t maxConnectionsPerOrigin = 8;
            std::size_t maxConcurrentStreamsPerConnection = 100;
            std::size_t maxOriginBytes = 256;
        };

        // The caller supplies a connected, already negotiated transport slot.
        // The pool stores only routing and lifecycle metadata; it never owns
        // sockets, TLS/QUIC engines, timers, queues, or event-loop threads.
        struct HttpConnectionDescriptor {
            std::string origin;
            HttpVersion version = HttpVersion::Http1;
            bool secure = false;
            bool datagram = false;
            std::size_t maxConcurrentStreams = 1;
        };

        struct HttpConnectionLease {
            std::uint64_t leaseId = 0;
            std::uint64_t connectionId = 0;
            std::uint64_t streamId = 0;
            std::uint64_t pathGeneration = 0;
            HttpVersion version = HttpVersion::Http1;
            bool secure = false;
            bool datagram = false;
        };

        struct HttpConnectionPoolSnapshot {
            std::size_t connections = 0;
            std::size_t readyConnections = 0;
            std::size_t drainingConnections = 0;
            std::size_t migratingConnections = 0;
            std::size_t activeLeases = 0;
            std::size_t availableSlots = 0;
        };

        // A serialized, caller-owned connection reuse policy. It selects an
        // already established slot and tracks stream leases; dialing,
        // multiplexed I/O, retry scheduling, and response delivery remain
        // outside this package.
        class LIKESPROGRAM_HTTP_API HttpConnectionPool {
        public:
            explicit HttpConnectionPool(HttpConnectionPoolLimits limits = {});
            ~HttpConnectionPool();

            HttpConnectionPool(HttpConnectionPool&&) noexcept;
            HttpConnectionPool& operator=(HttpConnectionPool&&) noexcept;
            HttpConnectionPool(const HttpConnectionPool&) = delete;
            HttpConnectionPool& operator=(const HttpConnectionPool&) = delete;

            Result<void> SetLimits(HttpConnectionPoolLimits limits);
            HttpConnectionPoolLimits Limits() const noexcept;

            Result<std::uint64_t> AddConnection(
                const HttpConnectionDescriptor& descriptor);
            Result<HttpConnectionLease> Acquire(
                std::string_view origin,
                HttpVersion version,
                bool secure,
                bool datagram);
            Result<void> Release(const HttpConnectionLease& lease);

            // GOAWAY/connection drain prevents new leases while existing
            // streams finish. The caller owns retry decisions for streams
            // above the peer's last accepted stream id.
            Result<void> MarkDraining(
                std::uint64_t connectionId,
                std::uint64_t lastStreamId);

            // H3 path migration is a two-phase caller transaction. The token
            // identifies an externally validated path; no packets are sent.
            Result<void> BeginMigration(
                std::uint64_t connectionId,
                std::uint64_t pathToken);
            Result<void> CommitMigration(
                std::uint64_t connectionId,
                std::uint64_t pathToken);
            Result<void> AbortMigration(
                std::uint64_t connectionId,
                std::uint64_t pathToken);

            // Close invalidates outstanding leases for this slot. Their
            // response/cancellation handling remains the caller's duty.
            Result<void> Close(std::uint64_t connectionId);

            HttpConnectionPoolSnapshot Snapshot() const noexcept;
            void Reset() noexcept;

        private:
            struct Impl;
            Impl* m_impl = nullptr;
        };
    }
}
