#pragma once
#include <iostream>
#include <thread>
#include <vector>
#include "../LikesProgram/time/Timer.hpp"
#include "../LikesProgram/time/Time.hpp"
#include "../LikesProgram/metrics/Summary.hpp"

using namespace LikesProgram;
using namespace LikesProgram::Time;
using namespace LikesProgram::Time::Convert;

namespace TimerTest {
	struct ThreadData {
        Timer* timer = nullptr;
		size_t index = 0;
	};

	void WorkLoad(ThreadData* data) {
		Timer threadTimer(true, data->timer); // 创建线程计时器

		// 模拟耗时操作
		std::this_thread::sleep_for(std::chrono::milliseconds(100 + data->index * 20));

		auto elapsed = threadTimer.Stop(); // 停止并获取耗时
		std::cout << "Thread 【" << data->index << "】：" << NsToMs(elapsed.count()) << "ms" << std::endl;

		delete data;
	}

    void Test() {
		// 创建 Summary 统计对象（可选）
		auto latencySummary = std::make_shared<LikesProgram::Metrics::Summary>(
			u"timer_duration_seconds",
			1024,
			u"Timer duration in seconds"
		);
		latencySummary->SetEMAAlpha(0.5);
		Timer timer(latencySummary); // 全局计时器

		std::cout << "===== 单线程示例 =====" << std::endl;
		{
			timer.Start(); // 开始计时
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

			//std::cout << "是否运行：" << timer.IsRunning() << std::endl;
			auto elapsed = timer.Stop();
            std::cout << "单线程：" << NsToMs(elapsed.count()) << "ms" << std::endl;

			std::cout << "是否运行：" << timer.IsRunning() << std::endl;
			std::cout << "最近一次耗时：" << NsToMs(elapsed.count()) << "ms" << std::endl;
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

			// 获取 Summary 统计对象
			const LikesProgram::Metrics::Summary summary = timer.GetSummary();
            std::cout << "Summary 统计结果：" << " 计时次数：" << summary.Count() << " ，总耗时：" << SToMs(summary.Sum()) << "ms" << std::endl;
			std::cout << "Prometheus：" << std::endl;
			std::wcout << summary.ToPrometheus() << std::endl;
			std::cout << "Json：" << std::endl;
			std::wcout << summary.ToJson() << std::endl;
		}

		std::cout << std::endl << "===== 重置示例 =====" << std::endl;
		{
			timer.Reset();

			const LikesProgram::Metrics::Summary summary = timer.GetSummary();
			std::cout << "Summary 统计结果：" << " 计时次数：" << summary.Count() << " ，总耗时：" << SToMs(summary.Sum()) << "ms" << std::endl;
			std::cout << "Prometheus：" << std::endl;
			std::wcout << summary.ToPrometheus() << std::endl;
			std::cout << "Json：" << std::endl;
			std::wcout << summary.ToJson() << std::endl;
		}
    }
}