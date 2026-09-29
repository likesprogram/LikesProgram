#include <LikesProgram/Net/BufferSlice.hpp>

namespace LikesProgram {
    namespace Net {
        BufferSlice::BufferSlice() noexcept = default;

        BufferSlice::BufferSlice(const std::uint8_t* data, std::size_t len) noexcept
            : m_data(data),
            m_len(data != nullptr ? len : 0) {
        }

        const std::uint8_t* BufferSlice::Data() const noexcept {
            return m_data;
        }

        std::size_t BufferSlice::Size() const noexcept {
            return m_len;
        }

        bool BufferSlice::Empty() const noexcept {
            return m_data == nullptr || m_len == 0;
        }

        std::string_view BufferSlice::AsStringView() const noexcept {
            return std::string_view(reinterpret_cast<const char*>(m_data), m_len);
        }
    }
}
