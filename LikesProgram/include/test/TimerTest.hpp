#pragma once
#include <iostream>
#include <thread>
#include <vector>
#include "../LikesProgram/Timer.hpp"

namespace TimerTest {
	struct ThreadData {
        LikesProgram::Timer* timer = nullptr;
		size_t index = 0;
	};

	void WorkLoad(ThreadData* data) {
		LikesProgram::Timer threadTimer(true, data->timer); // 创建线程计时器

		// 模拟耗时操作
		std::this_thread::sleep_for(std::chrono::milliseconds(100 + data->index * 20));

		auto elapsed = threadTimer.Stop(); // 停止并获取耗时
        std::cout << "Thread 【" << data->index << "】：" << LikesProgram::Timer::ToString(elapsed) << std::endl;
	}

    void Test() {
		LikesProgram::Timer timer; // 全局计时器

		std::cout << "===== 单线程示例 =====" << std::endl;
		{
			timer.Start(); // 开始计时
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

			//std::cout << "是否运行：" << timer.IsRunning() << std::endl;
			auto elapsed = timer.Stop();
            std::cout << "单线程：" << LikesProgram::Timer::ToString(elapsed) << std::endl;

			std::cout << "是否运行：" << timer.IsRunning() << std::endl;
			std::cout << "最近一次耗时：" << LikesProgram::Timer::ToString(timer.GetLastElapsed()) << std::endl;
		}

		std::cout << std::endl << "===== 多线程示例 =====" << std::endl;
		{
			const int threadCount = 4;
            std::vector<std::thread> threads;

			// 创建多个线程
			for (size_t i = 0; i < threadCount; i++) {
				ThreadData* data = new ThreadData();
				data->index = i;
                data->timer = &timer;
				threads.emplace_back(WorkLoad, data);
			}

			for (auto& thread : threads) thread.join();
			std::cout << std::endl << "===== 测试结果 =====" << std::endl;
			std::cout << "线程总时间：" << LikesProgram::Timer::ToString(timer.GetTotalElapsed()) << std::endl;
			std::cout << "最长时间：" << LikesProgram::Timer::ToString(timer.GetLongestElapsed()) << std::endl;
			std::cout << "EMA平均时间：" << LikesProgram::Timer::ToString(timer.GetEMAAverageElapsed()) << std::endl;
			std::cout << "线程算数平均时间：" << LikesProgram::Timer::ToString(timer.GetArithmeticAverageElapsed()) << std::endl;
		}

		std::cout << std::endl << "===== 重置示例 =====" << std::endl;
		{
			timer.Reset();

			std::cout << "线程总时间：" << LikesProgram::Timer::ToString(timer.GetTotalElapsed()) << std::endl;
			std::cout << "最长时间：" << LikesProgram::Timer::ToString(timer.GetLongestElapsed()) << std::endl;
			std::cout << "EMA平均时间：" << LikesProgram::Timer::ToString(timer.GetEMAAverageElapsed()) << std::endl;
			std::cout << "线程算数平均时间：" << LikesProgram::Timer::ToString(timer.GetArithmeticAverageElapsed()) << std::endl;
		}
    }
}