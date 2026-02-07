#include "../../../include/LikesProgram/net/Poller.hpp"
#include "../../../include/LikesProgram/net/EventLoop.hpp"
#include "../../../include/LikesProgram/net/Channel.hpp"

namespace LikesProgram {
	namespace Net {
		void Poller::SetEventLoop(EventLoop* ownerLoop) {
			if (m_ownerLoop != nullptr) return; // 不容许重定向
			m_ownerLoop = ownerLoop;
		}

		bool Poller::HasChannel(const Channel* ch) const {
			if (!ch) return false;
			const FdKey k = ToKey(ch->GetSocket());
			auto it = m_channels.find(k);
			return it != m_channels.end() && it->second == ch;
		}

		Poller::FdKey Poller::ToKey(SocketType fd) noexcept { return static_cast<FdKey>(fd); }

		void Poller::AssertInLoopThread() const noexcept {
			// 允许 ownerLoop 为空（便于先构造 poller，再由 EventLoop 接管线程语义）
			if (!m_ownerLoop) return;
			assert(m_ownerLoop->IsInLoopThread() && "Poller used outside its owner EventLoop thread");
		}
	}
}
