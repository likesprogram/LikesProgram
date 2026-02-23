#pragma once
#include "Buffer.hpp"
#include "SocketType.hpp"
#include <memory>

namespace LikesProgram {
	namespace Net {
		class MainEventLoop;
		class Broadcast {
		public:
			Broadcast() = default;
			~Broadcast() = default;

			// 发送数据
			void Send(const Buffer& buf);
			void Send(const Buffer& buf, const SocketType removeSocket);
			void Send(const Buffer& buf, const std::vector<SocketType>& removeSockets);

			// 发送数据
			void Send(const void* data, size_t len);
			void Send(const void* data, size_t len, const SocketType removeSocket);
			void Send(const void* data, size_t len, const std::vector<SocketType>& removeSockets);
		private:
			friend class MainEventLoop;
			std::weak_ptr<MainEventLoop> m_mainLoop;
		};
	}
}