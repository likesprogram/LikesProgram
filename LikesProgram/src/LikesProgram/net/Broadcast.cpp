#include "../../../include/LikesProgram/net/Broadcast.hpp"
#include "../../../include/LikesProgram/net/MainEventLoop.hpp"

namespace LikesProgram {
	namespace Net {
        void Broadcast::Send(const Buffer& buf) {
            Send(buf.Peek(), buf.ReadableBytes());
        }

        void Broadcast::Send(const Buffer& buf, SocketType removeSocket) {
            Send(buf.Peek(), buf.ReadableBytes(), removeSocket);
        }

        void Broadcast::Send(const Buffer& buf, const std::vector<SocketType>& removeSockets) {
            Send(buf.Peek(), buf.ReadableBytes(), removeSockets);
        }

        void Broadcast::Send(const void* data, size_t len) {
            static const std::vector<SocketType> kEmpty;
            Send(data, len, kEmpty);
        }

        void Broadcast::Send(const void* data, size_t len, SocketType removeSocket) {
            std::vector<SocketType> tmp;
            tmp.reserve(1);
            tmp.push_back(removeSocket);
            Send(data, len, tmp);
        }

        void Broadcast::Send(const void* data, size_t len, const std::vector<SocketType>& removeSockets) {
            auto mainLoop = m_mainLoop.lock();
            if (!mainLoop) return;

            auto loops = mainLoop->GetSubLoops();
            for (const auto& loopSp : loops) {
                if (!loopSp) continue;
                loopSp->BroadcastLocalExcept(data, len, removeSockets);
            }
        }
	}
}