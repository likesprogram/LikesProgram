#pragma once
#include <iostream>
#include <thread>
#include <vector>
#include "../LikesProgram/Timer.hpp"

namespace TimerTest {
	void WorkLoad(size_t id) {
		LikesProgram::Timer::Start(); // 开始计时

		// 模拟耗时操作
		std::this_thread::sleep_for(std::chrono::milliseconds(100 + id * 250));

		auto elapsed = LikesProgram::Timer::Stop(); // 停止并获取耗时
        std::cout << "Thread 【" << id << "】：" << LikesProgram::Timer::ToString(elapsed) << std::endl;
	}

    void Test() {
		std::cout << "===== 单线程示例 =====" << std::endl;
		{
            LikesProgram::Timer timer(true); // 构造并启动
            std::this_thread::sleep_for(std::chrono::milliseconds(250));

			std::cout << "是否运行：" << LikesProgram::Timer::IsRunning() << std::endl;
			auto elapsed = LikesProgram::Timer::Stop();
            std::cout << "单线程：" << LikesProgram::Timer::ToString(elapsed) << std::endl;

			std::cout << "是否运行：" << LikesProgram::Timer::IsRunning() << std::endl;
			std::cout << "最近一次耗时：" << LikesProgram::Timer::ToString(LikesProgram::Timer::GetLastElapsed()) << std::endl;
		}

		std::cout << std::endl << "===== 多线程示例 =====" << std::endl;
		{
			const int threadCount = 4;
            std::vector<std::thread> threads;

			// 创建多个线程
			for (size_t i = 0; i < threadCount; i++) {
				threads.emplace_back(WorkLoad, i);
			}

			for (auto& thread : threads) thread.join();

			std::cout << std::endl << "===== 测试结果 =====" << std::endl;
            std::cout << "线程总时间：" << LikesProgram::Timer::ToString(LikesProgram::Timer::GetTotalElapsed()) << std::endl;
			std::cout << "总时间：" << LikesProgram::Timer::ToString(LikesProgram::Timer::GetTotalGlobalElapsed()) << std::endl;
            std::cout << "最长时间：" << LikesProgram::Timer::ToString(LikesProgram::Timer::GetLongestElapsed()) << std::endl;
            std::cout << "EMA平均时间：" << LikesProgram::Timer::ToString(LikesProgram::Timer::GetEMAAverageElapsed()) << std::endl;
			std::cout << "线程算数平均时间：" << LikesProgram::Timer::ToString(LikesProgram::Timer::GetArithmeticAverageElapsed()) << std::endl;
			std::cout << "全局算数平均时间：" << LikesProgram::Timer::ToString(LikesProgram::Timer::GetArithmeticAverageGlobalElapsed()) << std::endl;
		}

		std::cout << std::endl << "===== 重置示例 =====" << std::endl;
		{
			LikesProgram::Timer::ResetThread(); // 重置当前线程计时数据
			LikesProgram::Timer::ResetGlobal(); // 重置全局计时数据

			std::cout << "线程总时间：" << LikesProgram::Timer::ToString(LikesProgram::Timer::GetTotalElapsed()) << std::endl;
			std::cout << "总时间：" << LikesProgram::Timer::ToString(LikesProgram::Timer::GetTotalGlobalElapsed()) << std::endl;
            std::cout << "最长时间：" << LikesProgram::Timer::ToString(LikesProgram::Timer::GetLongestElapsed()) << std::endl;
			std::cout << "EMA平均时间：" << LikesProgram::Timer::ToString(LikesProgram::Timer::GetEMAAverageElapsed()) << std::endl;
			std::cout << "线程算数平均时间：" << LikesProgram::Timer::ToString(LikesProgram::Timer::GetArithmeticAverageElapsed()) << std::endl;
			std::cout << "全局算数平均时间：" << LikesProgram::Timer::ToString(LikesProgram::Timer::GetArithmeticAverageGlobalElapsed()) << std::endl;
		}
    }
}