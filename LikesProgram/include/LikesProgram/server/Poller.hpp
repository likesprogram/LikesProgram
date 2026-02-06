#pragma once
#include "Channel.hpp"
#include <unordered_map>

namespace LikesProgram {
	namespace Server {
		class EventLoop; // 前向声明

		class Poller {
		public:
			Poller(){}
			virtual ~Poller() = 0;
			// 设置EventLoop
			void SetEventLoop(EventLoop* evLoop) { m_evLoop = evLoop; }
			// 添加
			virtual bool AddChannel(Channel* channel) = 0;
			// 删除
			virtual bool RemoveChannel(Channel* channel) = 0;
			// 修改
			virtual bool UpdateChannel(Channel* channel) = 0;
			// 事件监测
			virtual std::vector<Channel*> Dispatch(int timeoutMs = 2) = 0;
		protected:
			EventLoop* m_evLoop = nullptr;
			std::unordered_map<int, Channel*> m_channels; // fd -> Channel 映射
		};
	}
}