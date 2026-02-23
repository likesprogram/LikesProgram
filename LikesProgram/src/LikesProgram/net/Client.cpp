#include "../../../include/LikesProgram/net/Client.hpp"

namespace LikesProgram {
	namespace Net {
		Client::Client(std::string host, unsigned short port, ConnectionFactory factory, size_t subLoopCount)
		: m_host(host), m_port(port) {
		}

		Client::~Client()
		{
		}
	}
}
