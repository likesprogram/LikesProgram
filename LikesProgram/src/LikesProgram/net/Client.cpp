#include "../../../include/LikesProgram/net/Client.hpp"
#include "../../../include/LikesProgram/net/IOEvent.hpp"
#include <stdexcept>
#include <string>
#include <vector>
#include <chrono>
#include <atomic>
#include <thread>
#include <iostream>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <io.h>
#pragma comment(lib, "ws2_32.lib")
#include "../../../include/LikesProgram/net/pollers/WindowsSelectPoller.hpp"
static int GetSockErr() { return ::WSAGetLastError(); }
//static bool IsWouldBlock(int e) { return e == WSAEWOULDBLOCK; }
static bool IsInProgress(int e) { return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS; }
static int SetNonBlockingSock(SocketType s) { u_long nb = 1; return ::ioctlsocket(s, FIONBIO, &nb); }
#else
#include <unistd.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>
#include <poll.h>
#include "../../../include/LikesProgram/net/pollers/EpollPoller.hpp"
static int GetSockErr() { return errno; }
//static bool IsWouldBlock(int e) { return e == EAGAIN || e == EWOULDBLOCK; }
static bool IsInProgress(int e) { return e == EINPROGRESS; }
static int SetNonBlockingSock(SocketType s) {
	int flags = ::fcntl(s, F_GETFL, 0);
	if (flags < 0) return -1;
	return ::fcntl(s, F_SETFL, flags | O_NONBLOCK);
}
#endif

namespace LikesProgram {
	namespace Net {
		static int GetSoError(SocketType fd) {
			int err = 0;
#ifdef _WIN32
			int len = (int)sizeof(err);
			::getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*)&err, &len);
#else
			socklen_t len = (socklen_t)sizeof(err);
			::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
#endif
			return err;
		}

#ifdef _WIN32
		struct WinsockGlobal;
		extern WinsockGlobal& EnsureWinsock();
#endif
		static inline void CloseSocket(SocketType fd) {
#ifdef _WIN32
			if (fd != kInvalidSocket)::closesocket(fd);
#else
			if (fd != kInvalidSocket)::close(fd);
#endif
		}

		static bool WaitConnectReady(SocketType fd, std::atomic_bool& stopFlag, int timeoutMs) {
			if (timeoutMs < 0) timeoutMs = 0;
#ifdef _WIN32
			// WSAPoll: timeout 单位毫秒；返回 >0 表示有事件，0 超时，SOCKET_ERROR 出错
			WSAPOLLFD pfd{};
			pfd.fd = fd;
			// 对 connect：等待“可写”或错误即可
			pfd.events = POLLOUT | POLLERR | POLLHUP | POLLNVAL;

			// 用单调时钟控制总超时
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

			while (!stopFlag.load(std::memory_order_acquire)) {
				int remainMs = 0;
				if (timeoutMs > 0) {
					auto now = std::chrono::steady_clock::now();
					if (now >= deadline) return false; // timeout
					remainMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
					if (remainMs < 0) remainMs = 0;
				}

				int r = ::WSAPoll(&pfd, 1, remainMs);
				if (r > 0) return true;      // 有事件 -> 交给 SO_ERROR 判断成功/失败
				if (r == 0) return false;    // timeout
				// r < 0
				int e = WSAGetLastError();
				if (e == WSAEINTR) continue; // 被信号打断（不常见，但可处理）
				return true;                 // 其他错误：也交给 SO_ERROR/后续逻辑处理（通常会失败）
			}
			return false;

#else
			// 非 Windows：使用 poll（更接近 WSAPoll 语义）
			struct pollfd pfd {};
			pfd.fd = (int)fd;
			pfd.events = POLLOUT | POLLERR | POLLHUP | POLLNVAL;

			const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

			while (!stopFlag.load(std::memory_order_acquire)) {
				int remainMs = 0;
				if (timeoutMs > 0) {
					auto now = std::chrono::steady_clock::now();
					if (now >= deadline) return false;
					remainMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
					if (remainMs < 0) remainMs = 0;
				}

				int r = ::poll(&pfd, 1, remainMs);
				if (r > 0) return true;
				if (r == 0) return false;
				if (errno == EINTR) continue;
				return true;
			}
			return false;
#endif
		}

		static PollerFactory DefaultPollerFactory() {
#if defined(_WIN32)
			return []() -> std::unique_ptr<Poller> { return std::make_unique<WindowsSelectPoller>(nullptr); };
#else
			return []() -> std::unique_ptr<Poller> { return std::make_unique<EpollPoller>(nullptr); };
#endif
		}

		Client::Client(const Address& remoteAddr, ConnectionFactory factory)
		: m_factory(factory) {
#ifdef _WIN32
			(void)EnsureWinsock();
#endif
			m_remoteAddrs = Address::Resolve(remoteAddr.Ip(), remoteAddr.Port());
		}

		Client::~Client() {
			Shutdown();
		}

		void Client::Start() {
			auto poller = DefaultPollerFactory()();
			if (!poller) throw std::runtime_error("Client: poller factory returned null");
			m_loop = std::make_shared<EventLoop>(std::move(poller));

			{
				std::lock_guard<std::mutex> lk(m_stateMutex);
				if (!m_loop) return;

				if (StatusAnyOf(Status::Connecting, Status::Connected, Status::Stopping)) return;
				SetStatus(Status::Connecting);
			}
			// 如果上一次线程还在，先 join（避免多次 Start 创建多个线程）
			if (m_loopThread.joinable()) {
				if (m_loopThread.get_id() != std::this_thread::get_id())
					m_loopThread.join();
				else
					return;
			}

			m_loopThread = std::thread([this]() {
				m_loop->Start();
				// loop 退出后，标记 stopped
				SetStatus(Status::Stopped);
			});

			// 发起连接：一定要投递到 loop 线程
			m_loop->PostTask([this]() {
				if (!StatusEquals(Status::Connecting)) return;
				BeginConnect();
			});
		}

		void Client::WaitShutdown() const noexcept {
			std::unique_lock<std::mutex> lk(m_stateMutex);

			m_stateCv.wait(lk, [this]() { return StatusEquals(Status::Stopped); });
		}

		void Client::Shutdown() {
			{
				std::lock_guard<std::mutex> lk(m_stateMutex);
				if (StatusAnyOf(Status::Stopping, Status::Stopped)) return;
				SetStatus(Status::Stopping);
			}

			// 停止 connect waiter
			if (m_connectWaiter.joinable()) {
				m_stopWaiter.store(true, std::memory_order_release);
				m_connectWaiter.join();
				m_stopWaiter.store(false, std::memory_order_release);
			}

			// 把真正的关闭逻辑投递到 loop 线程（确保不和 poller 并发）
			if (m_loop) {
				m_loop->PostTask([this]() {
					// 关闭正在连接的 fd
					if (m_connectFd != kInvalidSocket) {
						CloseSocket(m_connectFd);
						m_connectFd = kInvalidSocket;
					}

					// 关闭已建立连接
					if (m_conn) {
						m_conn->Shutdown(); // 关闭
						m_conn.reset();
					}

					// 停止事件循环
					m_loop->Shutdown();
				});
			}

			if (m_loopThread.joinable() && m_loopThread.get_id() != std::this_thread::get_id()) {
				m_loopThread.join();
			}
			if (m_loop) m_loop.reset();
		}

		Client::Status Client::GetStatus() const noexcept {
			return m_status.load(std::memory_order_acquire);
		}

		std::shared_ptr<Connection> Client::GetConnection() const noexcept {
			std::lock_guard<std::mutex> lk(m_stateMutex);
			return m_conn;
		}

		void Client::BeginConnect() {
			if (!m_loop) {
				FinishConnectFail(0, "No Loop");
				return;
			}
			if (!StatusEquals(Status::Connecting)) return;

			addrinfo* result = nullptr;

			// 逐个地址尝试 connect
			SocketType fd = kInvalidSocket;
			int lastErr = 0;
			for (auto& addr : m_remoteAddrs) {
				fd = ::socket(addr.FamilyValue(), SOCK_STREAM, IPPROTO_TCP);
				if (fd == kInvalidSocket) { lastErr = GetSockErr(); continue; }

				if (SetNonBlockingSock(fd) != 0) {
					lastErr = GetSockErr();
					CloseSocket(fd);
					fd = kInvalidSocket;
					continue;
				}

				int rc = ::connect(fd, addr.SockAddr(), addr.Length());
				if (rc == 0) {
					// 立刻成功
					m_connectFd = fd;
					::freeaddrinfo(result);
					FinishConnectSuccess();
					return;
				}

				lastErr = GetSockErr();
				if (IsInProgress(lastErr)) {
					m_connectFd = fd;
					::freeaddrinfo(result);

					auto loop = m_loop;
					m_stopWaiter.store(false, std::memory_order_release);

					m_connectWaiter = std::thread([this, loop, fd]() {
						bool ready = WaitConnectReady(fd, m_stopWaiter, /*timeoutMs*/10000);
						if (m_stopWaiter.load(std::memory_order_acquire)) return;

						// 回到 loop 线程：OnConnectEvent 里 getsockopt(SO_ERROR) 判定成功/失败
						loop->PostTask([this, ready]() {
							if (!ready) {
								// timeout
								FinishConnectFail(/*err*/0, "connect timeout");
								return;
							}
							OnConnectEvent();
						});
					});
					return;
				}

				// 该地址失败，尝试下一个
				CloseSocket(fd);
				fd = kInvalidSocket;
			}

			::freeaddrinfo(result);
			FinishConnectFail(lastErr, "connect failed for all addrinfo");
		}

		void Client::OnConnectEvent() {
			if (!StatusEquals(Status::Connecting)) return;
			if (m_connectFd == kInvalidSocket) {
				FinishConnectFail(0, "connect fd invalid");
				return;
			}

			const int soerr = GetSoError(m_connectFd);
			if (soerr == 0) {
				FinishConnectSuccess();
			}
			else {
				FinishConnectFail(soerr, "connect failed (SO_ERROR)");
			}
		}

		void Client::FinishConnectSuccess() {
			if (!StatusEquals(Status::Connecting)) return;
			if (m_connectFd == kInvalidSocket) {
				FinishConnectFail(0, "FinishConnectSuccess: connect fd invalid");
				return;
			}

			// 1) 生成 Connection
			std::shared_ptr<Connection> conn;
			try {
				conn = m_factory(m_connectFd, m_loop.get());
			}
			catch (const std::exception& e) {
				FinishConnectFail(-1, e.what());
				return;
			}
			if (!conn) {
				FinishConnectFail(-1, "ConnectionFactory returned null");
				return;
			}

			// 2) 让 EventLoop 持有该连接（否则事件不会派发）
			m_loop->AttachConnection(conn);

			// 3) 建立 Channel 并注册（把所有权交给 Connection）
			{
				// 注意：Channel 通常需要 Connection* 用于 HandleEvent 分发
				auto ch = std::make_unique<Channel>(m_loop.get(), m_connectFd,
					IOEvent::Read | IOEvent::Error,
					conn.get());
				m_loop->RegisterChannel(ch.get());
				conn->AdoptChannel(std::move(ch));
			}

			// 4) Connection 开始工作（触发 OnConnected）
			conn->Start();

			// 5) 保存 conn（线程安全）
			{
				std::lock_guard<std::mutex> lk(m_stateMutex);
				m_conn = conn;
			}

			// 6) connect fd 现在已归 Connection 管理，Client 不再视为 “connect 阶段 fd”
			m_connectFd = kInvalidSocket;

			// 7) 状态切换 + 重连退避清零
			m_reconnectDelay = std::chrono::milliseconds(0);
			SetStatus(Status::Connected);
		}

		void Client::FinishConnectFail(int err, const char* why) {
			// 清理 connect fd
			if (m_connectFd != kInvalidSocket) {
				CloseSocket(m_connectFd);
				m_connectFd = kInvalidSocket;
			}

			// 如果已经在 stopping，就别重连
			if (StatusEquals(Status::Stopping)) {
				// 让 loop 自己退出（Shutdown 会 PostTask 调用 m_loop->Shutdown）
				return;
			}

			// 失败后仍处于 Connecting 语义：准备重连
			// 这里不要 SetStatus(Stopped)，因为 loop 还在跑
			SetStatus(Status::Connecting);

			(void)err; (void)why; // 你可以接 Logger 打点

			std::cout << err << ":" << why << std::endl;

			ScheduleReconnect();
		}

		void Client::ScheduleReconnect() {
			//if (!m_loop) return;
			//if (StatusEquals(Status::Stopping)) return;

			//// 计算退避
			//if (m_reconnectDelay.count() <= 0) m_reconnectDelay = std::chrono::milliseconds(200);
			//else {
			//	auto next = m_reconnectDelay * 2;
			//	const auto cap = std::chrono::seconds(30);
			//	m_reconnectDelay = (next > cap) ? cap : next;
			//}

			//// 复用 connectWaiter：确保旧线程结束
			//if (m_connectWaiter.joinable()) {
			//	m_stopWaiter.store(true, std::memory_order_release);
			//	m_connectWaiter.join();
			//	m_stopWaiter.store(false, std::memory_order_release);
			//}

			//auto loop = m_loop;
			//const auto delay = m_reconnectDelay;

			//m_stopWaiter.store(false, std::memory_order_release);
			//m_connectWaiter = std::thread([this, loop, delay]() {
			//	const auto deadline = std::chrono::steady_clock::now() + delay;
			//	while (!m_stopWaiter.load(std::memory_order_acquire)) {
			//		auto now = std::chrono::steady_clock::now();
			//		if (now >= deadline) break;
			//		std::this_thread::sleep_for(std::chrono::milliseconds(20));
			//	}
			//	if (m_stopWaiter.load(std::memory_order_acquire)) return;

			//	loop->PostTask([this]() {
			//		if (!StatusEquals(Status::Connecting)) return;
			//		BeginConnect();
			//	});
			//});
		}

		void Client::SetStatus(Status status) {
			m_status.store(status, std::memory_order_release);
			m_stateCv.notify_all();
		}

		bool Client::StatusEquals(Status status) const noexcept {
			return m_status.load(std::memory_order_acquire) == status;
		}
	}
}
