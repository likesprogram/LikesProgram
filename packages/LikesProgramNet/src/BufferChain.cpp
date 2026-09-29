#include <LikesProgram/Net/BufferChain.hpp>
#include <algorithm>
#include <utility>
#include <vector>

namespace LikesProgram {
    namespace Net {
        struct BufferChain::BufferChainImpl {
            std::vector<Buffer> m_segments;     // 按接收或转换顺序拥有全部 Buffer 段
            std::size_t m_firstReadable = 0;    // 第一个仍有可读字节的逻辑段索引
            std::size_t m_readableBytes = 0;    // 全链未消费字节总数
        };

        BufferChain::BufferChain()
            : m_impl(new BufferChainImpl{}) {
        }

        BufferChain::BufferChain(BufferChain&& other) noexcept
            : m_impl(other.m_impl) {
            other.m_impl = nullptr;
        }

        BufferChain::~BufferChain() {
            delete m_impl;
            m_impl = nullptr;
        }

        BufferChain& BufferChain::operator=(BufferChain&& other) noexcept {
            if (this == &other) return *this;

            delete m_impl;
            m_impl = other.m_impl;
            other.m_impl = nullptr;
            return *this;
        }

        std::size_t BufferChain::ReadableBytes() const noexcept {
            return m_impl != nullptr ? m_impl->m_readableBytes : 0;
        }

        std::size_t BufferChain::SegmentCount() const noexcept {
            return m_impl != nullptr ? m_impl->m_segments.size() - m_impl->m_firstReadable : 0;
        }

        bool BufferChain::Empty() const noexcept {
            return ReadableBytes() == 0;
        }

        BufferSlice BufferChain::Segment(std::size_t index) const noexcept {
            if (m_impl == nullptr || index >= SegmentCount()) return {};

            const Buffer& segment = m_impl->m_segments[m_impl->m_firstReadable + index]; // owner 保持视图有效
            return BufferSlice(segment.Peek(), segment.ReadableBytes());
        }

        void BufferChain::Append(Buffer&& buffer) {
            const std::size_t len = buffer.ReadableBytes(); // 移动前保存新段可读字节数
            if (len == 0) return;
            if (m_impl == nullptr) m_impl = new BufferChainImpl{};

            m_impl->m_segments.push_back(std::move(buffer));
            m_impl->m_readableBytes += len;
        }

        void BufferChain::Append(BufferLease&& lease) {
            if (lease.Empty()) return;

            Buffer segment(0); // lease 段只保留 prepend 空间，不预分配普通 1 KiB payload
            segment.Append(std::move(lease));
            Append(std::move(segment));
        }

        void BufferChain::Append(BufferChain&& chain) {
            if (this == &chain || chain.Empty()) return;
            if (m_impl == nullptr) m_impl = new BufferChainImpl{};

            const std::size_t incomingSegments = chain.SegmentCount(); // 尚未消费的待接管段数
            const std::size_t incomingBytes = chain.ReadableBytes(); // 移动前保存全链字节数
            m_impl->m_segments.reserve(m_impl->m_segments.size() + incomingSegments);
            for (std::size_t index = chain.m_impl->m_firstReadable;
                index < chain.m_impl->m_segments.size();
                ++index) {
                // Buffer 移动只转移 PImpl/lease 所有权，不复制 payload。
                m_impl->m_segments.push_back(std::move(chain.m_impl->m_segments[index]));
            }
            m_impl->m_readableBytes += incomingBytes;
            chain.Clear();
        }

        void BufferChain::Consume(std::size_t len) noexcept {
            if (m_impl == nullptr || len == 0 || m_impl->m_readableBytes == 0) return;

            std::size_t remaining = std::min(len, m_impl->m_readableBytes); // 本轮仍需跨段消费的字节数
            while (remaining > 0 && m_impl->m_firstReadable < m_impl->m_segments.size()) {
                Buffer& segment = m_impl->m_segments[m_impl->m_firstReadable]; // 当前最前可读段
                const std::size_t consumed = std::min(remaining, segment.ReadableBytes());
                segment.Consume(consumed);
                remaining -= consumed;
                m_impl->m_readableBytes -= consumed;
                if (segment.ReadableBytes() == 0) ++m_impl->m_firstReadable;
            }

            if (m_impl->m_readableBytes == 0) {
                Clear();
                return;
            }
            constexpr std::size_t kCompactThreshold = 16; // 避免短链频繁搬移，同时限制长流前缀增长
            if (m_impl->m_firstReadable >= kCompactThreshold
                && m_impl->m_firstReadable * 2 >= m_impl->m_segments.size()) {
                // Buffer 移动为 noexcept，压缩只转移 PImpl 指针，不复制 payload。
                m_impl->m_segments.erase(
                    m_impl->m_segments.begin(),
                    m_impl->m_segments.begin() + static_cast<std::ptrdiff_t>(m_impl->m_firstReadable));
                m_impl->m_firstReadable = 0;
            }
        }

        void BufferChain::Materialize() {
            if (m_impl == nullptr) return;

            // 只处理仍可见的段，已消费空段不再持有 lease。
            for (std::size_t index = m_impl->m_firstReadable;
                index < m_impl->m_segments.size();
                ++index) {
                m_impl->m_segments[index].Materialize();
            }
        }

        void BufferChain::Clear() noexcept {
            if (m_impl == nullptr) return;

            m_impl->m_segments.clear();
            m_impl->m_firstReadable = 0;
            m_impl->m_readableBytes = 0;
        }
    }
}
