#include <LikesProgram/Http/HttpConnectionPool.hpp>

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

namespace LikesProgram {
    namespace Http {
        namespace {
            struct PoolLease {
                std::uint64_t id = 0;
                std::uint64_t streamId = 0;
            };

            struct PoolConnection {
                std::uint64_t id = 0;
                std::string origin;
                HttpVersion version = HttpVersion::Http1;
                bool secure = false;
                bool datagram = false;
                std::size_t maxConcurrentStreams = 1;
                HttpConnectionState state = HttpConnectionState::Ready;
                std::uint64_t nextStreamId = 0;
                std::uint64_t goawayLastStream =
                    std::numeric_limits<std::uint64_t>::max();
                std::uint64_t pendingPathToken = 0;
                std::uint64_t pathGeneration = 0;
                std::vector<PoolLease> activeLeases;
            };

            bool IsSupportedVersion(HttpVersion version) noexcept {
                return version == HttpVersion::Http1
                    || version == HttpVersion::Http2
                    || version == HttpVersion::Http3;
            }

            PoolConnection* FindConnection(
                std::vector<PoolConnection>& connections,
                std::uint64_t id) noexcept {
                for (auto& connection : connections) {
                    if (connection.id == id) return &connection;
                }
                return nullptr;
            }

        }

        struct HttpConnectionPool::Impl {
            HttpConnectionPoolLimits limits{};
            std::uint64_t nextConnectionId = 1;
            std::uint64_t nextLeaseId = 1;
            std::vector<PoolConnection> connections;
        };

        HttpConnectionPool::HttpConnectionPool(HttpConnectionPoolLimits limits)
            : m_impl(new Impl{}) {
            if (limits.maxConnections != 0
                && limits.maxConnectionsPerOrigin != 0
                && limits.maxConcurrentStreamsPerConnection != 0
                && limits.maxOriginBytes != 0) {
                m_impl->limits = limits;
            }
        }

        HttpConnectionPool::~HttpConnectionPool() {
            delete m_impl;
        }

        HttpConnectionPool::HttpConnectionPool(HttpConnectionPool&& other) noexcept
            : m_impl(other.m_impl) {
            other.m_impl = nullptr;
        }

        HttpConnectionPool& HttpConnectionPool::operator=(
            HttpConnectionPool&& other) noexcept {
            if (this != &other) {
                delete m_impl;
                m_impl = other.m_impl;
                other.m_impl = nullptr;
            }
            return *this;
        }

        Result<void> HttpConnectionPool::SetLimits(HttpConnectionPoolLimits limits) {
            if (!m_impl || limits.maxConnections == 0
                || limits.maxConnectionsPerOrigin == 0
                || limits.maxConcurrentStreamsPerConnection == 0
                || limits.maxOriginBytes == 0) {
                return Status::InvalidArgument(u"HTTP connection pool limits must be non-zero");
            }
            if (m_impl->connections.size() > limits.maxConnections) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP connection pool cannot shrink below active connections");
            }
            for (const auto& connection : m_impl->connections) {
                if (connection.origin.size() > limits.maxOriginBytes
                    || connection.maxConcurrentStreams
                        > limits.maxConcurrentStreamsPerConnection) {
                    return Status(StatusCode::FailedPrecondition,
                        u"HTTP connection pool limits would invalidate a connection");
                }
                std::size_t sameOrigin = 0;
                for (const auto& candidate : m_impl->connections) {
                    if (candidate.origin == connection.origin) ++sameOrigin;
                }
                if (sameOrigin > limits.maxConnectionsPerOrigin) {
                    return Status(StatusCode::FailedPrecondition,
                        u"HTTP connection pool cannot shrink below an origin count");
                }
            }
            m_impl->limits = limits;
            return {};
        }

        HttpConnectionPoolLimits HttpConnectionPool::Limits() const noexcept {
            return m_impl ? m_impl->limits : HttpConnectionPoolLimits{};
        }

        Result<std::uint64_t> HttpConnectionPool::AddConnection(
            const HttpConnectionDescriptor& descriptor) {
            if (!m_impl || descriptor.origin.empty()
                || descriptor.origin.size() > m_impl->limits.maxOriginBytes) {
                return Status::InvalidArgument(u"HTTP connection origin is empty or too long");
            }
            if (!IsSupportedVersion(descriptor.version)) {
                return Status::InvalidArgument(u"HTTP connection version is unsupported");
            }
            if (descriptor.version == HttpVersion::Http3
                ? !descriptor.datagram
                : descriptor.datagram) {
                return Status::InvalidArgument(
                    u"HTTP/3 requires datagram=true and HTTP/1/2 require datagram=false");
            }
            if (descriptor.maxConcurrentStreams == 0
                || descriptor.maxConcurrentStreams
                    > m_impl->limits.maxConcurrentStreamsPerConnection
                || (descriptor.version == HttpVersion::Http1
                    && descriptor.maxConcurrentStreams != 1)) {
                return Status::InvalidArgument(
                    u"HTTP connection stream capacity is outside pool limits");
            }
            if (m_impl->connections.size() >= m_impl->limits.maxConnections) {
                return Status(StatusCode::ResourceExhausted,
                    u"HTTP connection pool has reached its connection limit");
            }
            std::size_t sameOrigin = 0;
            for (const auto& connection : m_impl->connections) {
                if (connection.origin == descriptor.origin) ++sameOrigin;
            }
            if (sameOrigin >= m_impl->limits.maxConnectionsPerOrigin) {
                return Status(StatusCode::ResourceExhausted,
                    u"HTTP connection pool has reached the origin limit");
            }
            if (m_impl->nextConnectionId == 0) {
                return Status(StatusCode::OutOfRange,
                    u"HTTP connection pool connection id exhausted");
            }

            PoolConnection connection;
            connection.id = m_impl->nextConnectionId++;
            connection.origin = descriptor.origin;
            connection.version = descriptor.version;
            connection.secure = descriptor.secure;
            connection.datagram = descriptor.datagram;
            connection.maxConcurrentStreams = descriptor.maxConcurrentStreams;
            connection.nextStreamId = descriptor.version == HttpVersion::Http2 ? 1 : 0;
            m_impl->connections.push_back(std::move(connection));
            return m_impl->connections.back().id;
        }

        Result<HttpConnectionLease> HttpConnectionPool::Acquire(
            std::string_view origin,
            HttpVersion version,
            bool secure,
            bool datagram) {
            if (!m_impl || origin.empty() || !IsSupportedVersion(version)) {
                return Status::InvalidArgument(u"HTTP connection acquire arguments are invalid");
            }
            PoolConnection* selected = nullptr;
            bool matching = false;
            bool blocked = false;
            bool readyMatching = false;
            for (auto& connection : m_impl->connections) {
                if (connection.origin != origin || connection.version != version
                    || connection.secure != secure || connection.datagram != datagram) {
                    continue;
                }
                matching = true;
                if (connection.state != HttpConnectionState::Ready) {
                    blocked = true;
                    continue;
                }
                readyMatching = true;
                if (connection.activeLeases.size() >= connection.maxConcurrentStreams) {
                    continue;
                }
                if (selected == nullptr
                    || connection.activeLeases.size() < selected->activeLeases.size()) {
                    selected = &connection;
                }
            }
            if (selected == nullptr) {
                if (!matching) {
                    return Status::NotFound(u"no matching HTTP connection is registered");
                }
                if (readyMatching) {
                    return Status(StatusCode::ResourceExhausted,
                        u"matching HTTP connections have no available stream slot");
                }
                if (blocked) {
                    return Status(StatusCode::FailedPrecondition,
                        u"matching HTTP connections are draining or migrating");
                }
                return Status(StatusCode::ResourceExhausted,
                    u"matching HTTP connections have no available stream slot");
            }

            const auto streamId = selected->nextStreamId;
            if (m_impl->nextLeaseId == 0) {
                return Status(StatusCode::OutOfRange,
                    u"HTTP connection pool lease id exhausted");
            }
            if (selected->version != HttpVersion::Http1) {
                const std::uint64_t increment = selected->version == HttpVersion::Http3
                    ? 4 : 2;
                if (streamId > std::numeric_limits<std::uint64_t>::max() - increment) {
                    return Status(StatusCode::OutOfRange,
                        u"HTTP stream id space is exhausted");
                }
                selected->nextStreamId += increment;
            }
            const auto leaseId = m_impl->nextLeaseId++;
            selected->activeLeases.push_back(PoolLease{leaseId, streamId});

            HttpConnectionLease lease;
            lease.leaseId = leaseId;
            lease.connectionId = selected->id;
            lease.streamId = streamId;
            lease.pathGeneration = selected->pathGeneration;
            lease.version = selected->version;
            lease.secure = selected->secure;
            lease.datagram = selected->datagram;
            return lease;
        }

        Result<void> HttpConnectionPool::Release(const HttpConnectionLease& lease) {
            if (!m_impl || lease.leaseId == 0 || lease.connectionId == 0) {
                return Status::InvalidArgument(u"HTTP connection lease is invalid");
            }
            auto* connection = FindConnection(m_impl->connections, lease.connectionId);
            if (connection == nullptr) {
                return Status::NotFound(u"HTTP connection lease refers to a closed connection");
            }
            if (connection->version != lease.version
                || connection->secure != lease.secure
                || connection->datagram != lease.datagram) {
                return Status::InvalidArgument(u"HTTP connection lease metadata does not match");
            }
            const auto it = std::find_if(
                connection->activeLeases.begin(), connection->activeLeases.end(),
                [&lease](const PoolLease& candidate) {
                    return candidate.id == lease.leaseId
                        && candidate.streamId == lease.streamId;
                });
            if (it == connection->activeLeases.end()) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP connection lease was already released");
            }
            connection->activeLeases.erase(it);
            return {};
        }

        Result<void> HttpConnectionPool::MarkDraining(
            std::uint64_t connectionId,
            std::uint64_t lastStreamId) {
            if (!m_impl || connectionId == 0) {
                return Status::InvalidArgument(u"HTTP connection id is invalid");
            }
            auto* connection = FindConnection(m_impl->connections, connectionId);
            if (connection == nullptr) {
                return Status::NotFound(u"HTTP connection was not found");
            }
            if (connection->state == HttpConnectionState::Closed) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP connection is already closed");
            }
            if (connection->state == HttpConnectionState::Migrating) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP connection migration must finish before draining");
            }
            if (connection->version == HttpVersion::Http1 && lastStreamId != 0) {
                return Status::InvalidArgument(u"HTTP/1 connection has no GOAWAY stream id");
            }
            if (connection->version != HttpVersion::Http1
                && lastStreamId > connection->goawayLastStream) {
                return Status(StatusCode::OutOfRange,
                    u"HTTP GOAWAY stream id must not increase");
            }
            connection->goawayLastStream = lastStreamId;
            connection->state = HttpConnectionState::Draining;
            return {};
        }

        Result<void> HttpConnectionPool::BeginMigration(
            std::uint64_t connectionId,
            std::uint64_t pathToken) {
            if (!m_impl || connectionId == 0 || pathToken == 0) {
                return Status::InvalidArgument(u"HTTP migration token or connection id is invalid");
            }
            auto* connection = FindConnection(m_impl->connections, connectionId);
            if (connection == nullptr) return Status::NotFound(u"HTTP connection was not found");
            if (connection->version != HttpVersion::Http3) {
                return Status(StatusCode::Unimplemented,
                    u"only HTTP/3 connections expose path migration");
            }
            if (connection->state != HttpConnectionState::Ready) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 connection is not ready for migration");
            }
            connection->pendingPathToken = pathToken;
            connection->state = HttpConnectionState::Migrating;
            return {};
        }

        Result<void> HttpConnectionPool::CommitMigration(
            std::uint64_t connectionId,
            std::uint64_t pathToken) {
            if (!m_impl || connectionId == 0 || pathToken == 0) {
                return Status::InvalidArgument(u"HTTP migration token or connection id is invalid");
            }
            auto* connection = FindConnection(m_impl->connections, connectionId);
            if (connection == nullptr) return Status::NotFound(u"HTTP connection was not found");
            if (connection->state != HttpConnectionState::Migrating
                || connection->pendingPathToken != pathToken) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP migration has no matching pending path");
            }
            if (connection->pathGeneration == std::numeric_limits<std::uint64_t>::max()) {
                return Status(StatusCode::OutOfRange, u"HTTP migration generation exhausted");
            }
            ++connection->pathGeneration;
            connection->pendingPathToken = 0;
            connection->state = HttpConnectionState::Ready;
            return {};
        }

        Result<void> HttpConnectionPool::AbortMigration(
            std::uint64_t connectionId,
            std::uint64_t pathToken) {
            if (!m_impl || connectionId == 0 || pathToken == 0) {
                return Status::InvalidArgument(u"HTTP migration token or connection id is invalid");
            }
            auto* connection = FindConnection(m_impl->connections, connectionId);
            if (connection == nullptr) return Status::NotFound(u"HTTP connection was not found");
            if (connection->state != HttpConnectionState::Migrating
                || connection->pendingPathToken != pathToken) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP migration has no matching pending path");
            }
            connection->pendingPathToken = 0;
            connection->state = HttpConnectionState::Draining;
            return {};
        }

        Result<void> HttpConnectionPool::Close(std::uint64_t connectionId) {
            if (!m_impl || connectionId == 0) {
                return Status::InvalidArgument(u"HTTP connection id is invalid");
            }
            const auto it = std::find_if(
                m_impl->connections.begin(), m_impl->connections.end(),
                [connectionId](const PoolConnection& connection) {
                    return connection.id == connectionId;
                });
            if (it == m_impl->connections.end()) {
                return Status::NotFound(u"HTTP connection was not found");
            }
            m_impl->connections.erase(it);
            return {};
        }

        HttpConnectionPoolSnapshot HttpConnectionPool::Snapshot() const noexcept {
            HttpConnectionPoolSnapshot snapshot;
            if (!m_impl) return snapshot;
            snapshot.connections = m_impl->connections.size();
            for (const auto& connection : m_impl->connections) {
                snapshot.activeLeases += connection.activeLeases.size();
                if (connection.state == HttpConnectionState::Ready) {
                    ++snapshot.readyConnections;
                    snapshot.availableSlots += connection.maxConcurrentStreams
                        - connection.activeLeases.size();
                } else if (connection.state == HttpConnectionState::Draining) {
                    ++snapshot.drainingConnections;
                } else if (connection.state == HttpConnectionState::Migrating) {
                    ++snapshot.migratingConnections;
                }
            }
            return snapshot;
        }

        void HttpConnectionPool::Reset() noexcept {
            if (!m_impl) return;
            m_impl->connections.clear();
        }
    }
}
