#include <LikesProgram/Net/DtlsDatagramBatch.hpp>
#include <deque>
#include <stdexcept>
#include <utility>

namespace LikesProgram {
    namespace Net {
        struct DtlsDatagramBatch::DtlsDatagramBatchImpl {
            std::deque<Buffer> m_datagrams; // 每个元素保持一个完整数据报边界
            std::size_t m_totalBytes = 0;   // 未取出数据报的总字节数
        };

        DtlsDatagramBatch::DtlsDatagramBatch()
            : m_impl(new DtlsDatagramBatchImpl{}) {
        }

        DtlsDatagramBatch::DtlsDatagramBatch(DtlsDatagramBatch&& other) noexcept
            : m_impl(other.m_impl) {
            // moved-from 对象保持可析构并允许后续重新 Append。
            other.m_impl = nullptr;
        }

        DtlsDatagramBatch::~DtlsDatagramBatch() {
            delete m_impl;
            m_impl = nullptr;
        }

        DtlsDatagramBatch& DtlsDatagramBatch::operator=(DtlsDatagramBatch&& other) noexcept {
            if (this == &other) return *this;

            // 先释放旧队列，再唯一接管源对象的 PImpl。
            delete m_impl;
            m_impl = other.m_impl;
            other.m_impl = nullptr;
            return *this;
        }

        std::size_t DtlsDatagramBatch::Count() const noexcept {
            return m_impl ? m_impl->m_datagrams.size() : 0;
        }

        std::size_t DtlsDatagramBatch::TotalBytes() const noexcept {
            return m_impl ? m_impl->m_totalBytes : 0;
        }

        bool DtlsDatagramBatch::Empty() const noexcept {
            return m_impl == nullptr || m_impl->m_datagrams.empty();
        }

        const Buffer& DtlsDatagramBatch::At(std::size_t index) const {
            if (m_impl == nullptr || index >= m_impl->m_datagrams.size()) {
                throw std::out_of_range("DTLS datagram batch index is out of range");
            }
            return m_impl->m_datagrams[index];
        }

        void DtlsDatagramBatch::Append(Buffer&& datagram) {
            if (m_impl == nullptr) m_impl = new DtlsDatagramBatchImpl{};

            // 先记录 payload 字节，再移动 Buffer 保留底层所有权地址。
            const std::size_t readableBytes = datagram.ReadableBytes(); // 当前完整数据报字节数
            m_impl->m_datagrams.push_back(std::move(datagram));
            m_impl->m_totalBytes += readableBytes;
        }

        bool DtlsDatagramBatch::TakeFront(Buffer& datagram) {
            if (m_impl == nullptr || m_impl->m_datagrams.empty()) return false;

            // 队首大小在移动前读取，避免依赖 moved-from Buffer 状态。
            const std::size_t readableBytes = m_impl->m_datagrams.front().ReadableBytes(); // 待扣除字节数
            datagram = std::move(m_impl->m_datagrams.front());
            m_impl->m_datagrams.pop_front();
            m_impl->m_totalBytes -= readableBytes;
            return true;
        }

        void DtlsDatagramBatch::Clear() noexcept {
            if (m_impl == nullptr) return;

            // 释放全部 Buffer/lease 后同步归零总字节快照。
            m_impl->m_datagrams.clear();
            m_impl->m_totalBytes = 0;
        }
    }
}
