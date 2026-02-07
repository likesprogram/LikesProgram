#pragma once
#include "Channel.hpp"
#include <unordered_map>
#include <vector>
#include <cassert>

namespace LikesProgram {
	namespace Net {
		class EventLoop; // 前向声明
		class Channel; // 前向声明

		class Poller {
		public:
			using FdKey = std::uintptr_t;

			explicit Poller() : m_ownerLoop(nullptr) {}
			explicit Poller(EventLoop* ownerLoop) : m_ownerLoop(ownerLoop) {}
			virtual ~Poller() = default;

			void SetEventLoop(EventLoop* ownerLoop);

			Poller(const Poller&) = delete;
			Poller& operator=(const Poller&) = delete;

			// 添加
			virtual bool AddChannel(Channel* channel) = 0;
			// 删除
			virtual bool RemoveChannel(Channel* channel) = 0;
			// 修改
			virtual bool UpdateChannel(Channel* channel) = 0;
			// 复用 active 容器，避免频繁分配
			virtual void Poll(int timeoutMs, std::vector<Channel*>& active) = 0;

			// 查询/诊断
			bool HasChannel(const Channel* ch) const;

			int LastError() const noexcept { return m_lastErr; }
		protected:
			static FdKey ToKey(SocketType fd) noexcept;

			void SetLastError(int e) noexcept { m_lastErr = e; }

			// 由 EventLoop 提供线程断言
			void AssertInLoopThread() const noexcept;
			
			EventLoop* m_ownerLoop{ nullptr };
			std::unordered_map<FdKey, Channel*> m_channels;
			int m_lastErr{ 0 };
		};
	}
}