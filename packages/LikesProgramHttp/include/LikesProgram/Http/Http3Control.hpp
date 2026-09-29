#pragma once

#include <LikesProgram/Http/system/LikesProgramHttpExport.hpp>
#include <LikesProgram/Http/Http3.hpp>
#include <LikesProgram/Core/Result.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace LikesProgram {
    namespace Http {
        inline constexpr std::uint64_t kHttp3ControlStreamType = 0x00;

        struct Http3Setting {
            std::uint64_t id = 0;
            std::uint64_t value = 0;

            friend bool operator==(const Http3Setting&, const Http3Setting&) = default;
        };

        enum class Http3ControlStreamState : std::uint8_t {
            AwaitingStreamType,
            AwaitingSettings,
            Open,
            Failed
        };

        struct Http3ControlStreamSnapshot {
            Http3ControlStreamState state = Http3ControlStreamState::AwaitingStreamType;
            bool streamTypeAccepted = false;
            bool settingsReceived = false;
            std::size_t settingsCount = 0;
            bool goawayReceived = false;
            std::uint64_t goawayId = 0;
            bool maxPushIdReceived = false;
            std::uint64_t maxPushId = 0;
            std::size_t cancelPushCount = 0;
            std::uint64_t lastCancelPushId = 0;
        };

        // Parse/build SETTINGS payloads without owning a QUIC stream.
        LIKESPROGRAM_HTTP_API Result<std::vector<Http3Setting>> ParseHttp3Settings(const std::vector<std::uint8_t>& payload);
        LIKESPROGRAM_HTTP_API Result<std::vector<std::uint8_t>> BuildHttp3Settings(const std::vector<Http3Setting>& settings);
        LIKESPROGRAM_HTTP_API Result<std::vector<std::uint8_t>> BuildHttp3ControlStreamType();
        LIKESPROGRAM_HTTP_API Result<std::vector<std::uint8_t>> BuildHttp3ControlStreamInitialBytes(const std::vector<Http3Setting>& settings);

        // Sans-I/O receiver-side control stream contract. The adapter supplies
        // the unidirectional stream type and already framed QUIC payloads.
        class Http3ControlStream {
        public:
            LIKESPROGRAM_HTTP_API explicit Http3ControlStream(std::size_t maxSettings = 64);
            LIKESPROGRAM_HTTP_API ~Http3ControlStream();

            LIKESPROGRAM_HTTP_API Http3ControlStream(Http3ControlStream&&) noexcept;
            LIKESPROGRAM_HTTP_API Http3ControlStream& operator=(Http3ControlStream&&) noexcept;
            Http3ControlStream(const Http3ControlStream&) = delete;
            Http3ControlStream& operator=(const Http3ControlStream&) = delete;

            LIKESPROGRAM_HTTP_API Result<void> AcceptStreamType(std::uint64_t streamType);
            LIKESPROGRAM_HTTP_API Result<void> Feed(const Http3Frame& frame);
            LIKESPROGRAM_HTTP_API const std::vector<Http3Setting>& Settings() const noexcept;
            LIKESPROGRAM_HTTP_API Http3ControlStreamSnapshot Snapshot() const noexcept;
            LIKESPROGRAM_HTTP_API Http3ControlStreamState State() const noexcept;
            LIKESPROGRAM_HTTP_API Status LastError() const;
            LIKESPROGRAM_HTTP_API void Reset() noexcept;

        private:
            struct Impl;
            std::unique_ptr<Impl> m_impl;
        };

        struct Http3ControlStreamWireLimits {
            std::size_t maxPendingBytes = 64 * 1024;
            std::size_t maxFramePayloadBytes = 64 * 1024;
        };

        // A control-stream failure closes the HTTP/3 connection. The caller
        // owns QUIC frame emission and connection lifetime.
        struct Http3ControlStreamQuicActions {
            std::uint64_t quicErrorCode = 0;
            bool closeConnection = false;
        };

        // Reassembles the control stream type and frames from arbitrary QUIC
        // stream chunks without owning the stream, socket, TLS or timer.
        class Http3ControlStreamWireDecoder {
        public:
            LIKESPROGRAM_HTTP_API explicit Http3ControlStreamWireDecoder(std::size_t maxSettings = 64, Http3ControlStreamWireLimits wireLimits = {});
            LIKESPROGRAM_HTTP_API ~Http3ControlStreamWireDecoder();

            LIKESPROGRAM_HTTP_API Http3ControlStreamWireDecoder(Http3ControlStreamWireDecoder&&) noexcept;
            LIKESPROGRAM_HTTP_API Http3ControlStreamWireDecoder& operator=(Http3ControlStreamWireDecoder&&) noexcept;
            Http3ControlStreamWireDecoder(const Http3ControlStreamWireDecoder&) = delete;
            Http3ControlStreamWireDecoder& operator=(const Http3ControlStreamWireDecoder&) = delete;

            LIKESPROGRAM_HTTP_API Result<void> Feed(const std::uint8_t* data, std::size_t size, bool endStream = false);
            LIKESPROGRAM_HTTP_API Result<void> Feed(const std::vector<std::uint8_t>& bytes, bool endStream = false);
            LIKESPROGRAM_HTTP_API Result<void> Finish();
            LIKESPROGRAM_HTTP_API Http3ControlStreamWireLimits WireLimits() const noexcept;
            LIKESPROGRAM_HTTP_API std::size_t PendingBytes() const noexcept;
            LIKESPROGRAM_HTTP_API Http3ControlStreamSnapshot Snapshot() const noexcept;
            LIKESPROGRAM_HTTP_API const std::vector<Http3Setting>& Settings() const noexcept;
            LIKESPROGRAM_HTTP_API Status LastError() const;
            LIKESPROGRAM_HTTP_API Result<Http3ControlStreamQuicActions> FailureActions() const;
            LIKESPROGRAM_HTTP_API void Reset() noexcept;

        private:
            struct Impl;
            std::unique_ptr<Impl> m_impl;
        };

        inline Result<void> Http3ControlStreamWireDecoder::Feed(const std::vector<std::uint8_t>& bytes, bool endStream) { return Feed(bytes.data(), bytes.size(), endStream); }
    }
}
