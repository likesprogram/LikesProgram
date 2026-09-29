#include <LikesProgram/Quic/QuicConnectionCloseEventMapping.hpp>

namespace LikesProgram {
    namespace Quic {
        Result<QuicStreamEvent> MapQuicConnectionCloseFrameToEvent(
            const QuicConnectionCloseFrame& frame,
            const Address& peer) {
            QuicStreamEvent event;
            event.kind = QuicEventKind::ConnectionClose;
            event.errorCode = frame.errorCode;
            event.applicationError = frame.application;
            event.peer = peer;
            if (!frame.reason.empty()) {
                event.payload = Buffer(0);
                event.payload.Append(frame.reason.data(), frame.reason.size());
            }
            return event;
        }
    }
}
