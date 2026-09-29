#include <LikesProgram/Quic/QuicStreamEventMapping.hpp>

namespace LikesProgram {
    namespace Quic {
        Result<QuicStreamEvent> MapQuicStreamFrameToEvent(
            const QuicStreamFrame& frame,
            const Address& peer) {
            QuicStreamEvent event;
            event.kind = frame.fin
                ? QuicEventKind::StreamFin
                : QuicEventKind::StreamData;
            event.streamId = frame.streamId;
            event.peer = peer;
            if (!frame.data.empty()) {
                event.payload = Buffer(0);
                event.payload.Append(frame.data.data(), frame.data.size());
            }
            return event;
        }
    }
}
