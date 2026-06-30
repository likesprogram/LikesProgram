#pragma once
#include <LikesProgram/Core/String.hpp>
#include <optional>

namespace LikesProgram {
    // 跨包通用状态码，扩展包可用它表达可恢复错误和边界结果。
    enum class StatusCode {
        Ok,
        Cancelled,
        InvalidArgument,
        NotFound,
        AlreadyExists,
        PermissionDenied,
        ResourceExhausted,
        FailedPrecondition,
        OutOfRange,
        Unimplemented,
        Internal,
        Unavailable,
        DeadlineExceeded,
        Unknown
    };

    // 返回状态码的稳定文本名称。
    inline String StatusCodeName(StatusCode code) {
        switch (code) {
        case StatusCode::Ok: return u"Ok";
        case StatusCode::Cancelled: return u"Cancelled";
        case StatusCode::InvalidArgument: return u"InvalidArgument";
        case StatusCode::NotFound: return u"NotFound";
        case StatusCode::AlreadyExists: return u"AlreadyExists";
        case StatusCode::PermissionDenied: return u"PermissionDenied";
        case StatusCode::ResourceExhausted: return u"ResourceExhausted";
        case StatusCode::FailedPrecondition: return u"FailedPrecondition";
        case StatusCode::OutOfRange: return u"OutOfRange";
        case StatusCode::Unimplemented: return u"Unimplemented";
        case StatusCode::Internal: return u"Internal";
        case StatusCode::Unavailable: return u"Unavailable";
        case StatusCode::DeadlineExceeded: return u"DeadlineExceeded";
        case StatusCode::Unknown: return u"Unknown";
        }
        return u"Unknown";
    }

    // 轻量状态对象，作为异常之外的公共返回契约。
    class Status {
    public:
        // 默认构造表示成功。
        Status() = default;

        // 使用状态码和可选消息构造状态。
        Status(StatusCode code, const String& message = String())
            : m_code(code) {
            if (!message.Empty()) m_message = message;
        }

        // 构造成功状态。
        static Status OkStatus() {
            return Status();
        }

        // 构造非法参数状态。
        static Status InvalidArgument(const String& message) {
            return Status(StatusCode::InvalidArgument, message);
        }

        // 构造未找到状态。
        static Status NotFound(const String& message) {
            return Status(StatusCode::NotFound, message);
        }

        // 构造内部错误状态。
        static Status Internal(const String& message) {
            return Status(StatusCode::Internal, message);
        }

        // 构造超时状态。
        static Status DeadlineExceeded(const String& message) {
            return Status(StatusCode::DeadlineExceeded, message);
        }

        // 当前状态是否成功。
        bool IsOk() const {
            return m_code == StatusCode::Ok;
        }

        // 允许在 if(status) 中表达成功判断。
        explicit operator bool() const {
            return IsOk();
        }

        // 返回状态码。
        StatusCode Code() const {
            return m_code;
        }

        // 返回状态消息。
        const String& Message() const {
            static const String emptyMessage; // 无消息状态共享空串，避免成功路径分配
            if (!m_message.has_value()) return emptyMessage;
            return *m_message;
        }

        // 返回状态码与消息组成的可读文本。
        String ToString() const {
            if (!m_message.has_value() || m_message->Empty()) return StatusCodeName(m_code);
            return String::Format(u"{}: {}", StatusCodeName(m_code), *m_message);
        }

    private:
        StatusCode m_code = StatusCode::Ok;        // 当前状态码，默认表示成功
        std::optional<String> m_message;            // 可选诊断消息，成功路径不分配 String
    };
}
