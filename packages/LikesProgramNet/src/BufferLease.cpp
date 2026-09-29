#include <LikesProgram/Net/BufferLease.hpp>

namespace LikesProgram {
    namespace Net {
        BufferLease::BufferLease() noexcept = default;

        BufferLease::BufferLease(
            std::uint8_t* data,
            std::size_t len,
            void* owner,
            std::uint32_t token,
            ReleaseFunction releaseFunction) noexcept
            : m_data(data),
            m_len(len),
            m_owner(owner),
            m_token(token),
            m_releaseFunction(releaseFunction) {
        }

        BufferLease::BufferLease(BufferLease&& other) noexcept
            : m_data(other.m_data),
            m_len(other.m_len),
            m_owner(other.m_owner),
            m_token(other.m_token),
            m_releaseFunction(other.m_releaseFunction) {
            // moved-from lease 必须保持为空，析构时不得重复归还同一 buffer。
            other.m_data = nullptr;
            other.m_len = 0;
            other.m_owner = nullptr;
            other.m_token = 0;
            other.m_releaseFunction = nullptr;
        }

        BufferLease::~BufferLease() {
            Reset();
        }

        BufferLease& BufferLease::operator=(BufferLease&& other) noexcept {
            if (this == &other) return *this;

            Reset();
            m_data = other.m_data;
            m_len = other.m_len;
            m_owner = other.m_owner;
            m_token = other.m_token;
            m_releaseFunction = other.m_releaseFunction;

            // 转移完成后清空源对象，保持归还职责唯一。
            other.m_data = nullptr;
            other.m_len = 0;
            other.m_owner = nullptr;
            other.m_token = 0;
            other.m_releaseFunction = nullptr;
            return *this;
        }

        const std::uint8_t* BufferLease::Data() const noexcept {
            return m_data;
        }

        std::size_t BufferLease::Size() const noexcept {
            return m_len;
        }

        bool BufferLease::Empty() const noexcept {
            return Data() == nullptr || Size() == 0;
        }

        void BufferLease::Reset() noexcept {
            if (m_data == nullptr && m_releaseFunction == nullptr) return;

            ReleaseFunction releaseFunction = m_releaseFunction; // 清空前保存无异常回调
            void* owner = m_owner; // 平台池上下文
            const std::uint32_t token = m_token; // 需要归还的 buffer id

            // 回调可能触发后续任务，调用前必须先让当前 lease 进入稳定空状态。
            m_data = nullptr;
            m_len = 0;
            m_owner = nullptr;
            m_token = 0;
            m_releaseFunction = nullptr;
            if (releaseFunction != nullptr) releaseFunction(owner, token);
        }
    }
}
