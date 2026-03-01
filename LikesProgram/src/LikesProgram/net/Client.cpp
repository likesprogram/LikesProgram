#include "../../../include/LikesProgram/net/Client.hpp"

namespace LikesProgram {
	namespace Net {
		Client::Client(const String& host, unsigned short port, ConnectionFactory factory, size_t subLoopCount)
		: m_host(host), m_port(port) {
		}

		Client::~Client()
		{
		}

		void Client::Start() {

		}

		void Client::Shutdown() {

		}

		Connection* Client::GetConnection() const noexcept {
			return m_conn.get();
		}

	}
}
