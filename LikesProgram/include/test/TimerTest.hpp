#pragma once
#include <iostream>
#include <thread>
#include <vector>
#include "../LikesProgram/time/Timer.hpp"
#include "../LikesProgram/time/Time.hpp"
#include "../LikesProgram/metrics/Summary.hpp"

using namespace LikesProgram;
using namespace LikesProgram::Time;

namespace TimerTest {
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

		std::cout << std::endl << "===== 拷贝示例 =====" << std::endl;
		{
			// 直接使用拷贝，拷贝后与当前计时器共享一个 Summary 记录
			// 这里演示拷贝，实际开发中建议使用指针避免拷贝，父子关系已经可以共享 Summary 的记录
            Timer timerCopy = timer;
			timerCopy.Start(); // 开始计时
			std::this_thread::sleep_for(std::chrono::milliseconds(100));

			auto elapsed = timerCopy.Stop();
			std::cout << "拷贝示例：" << NsToMs(elapsed.count()) << "ms" << std::endl;
			std::cout << "最近一次耗时：" << NsToMs(elapsed.count()) << "ms" << std::endl;
		}

		std::cout << std::endl << "===== 多线程示例 =====" << std::endl;
		{
			const int threadCount = 4;
            std::vector<std::thread> threads;

			// 创建多个线程
			for (size_t i = 0; i < threadCount; i++) {
				threads.emplace_back([i, &timer]() {
					// 为了避免线程竞争 Start 与 Stop 的数据，因此这里需要使用父子关系
					// 创建线程计时器，创建时指定父计时器，记录器会同步统计到父计时器中
					Timer threadTimer(true, &timer); // 创建线程计时器

					// 模拟耗时操作
					std::this_thread::sleep_for(std::chrono::milliseconds(100 + i * 20));

					auto elapsed = threadTimer.Stop(); // 停止并获取耗时
					std::cout << "Thread 【" << i << "】：" << NsToMs(elapsed.count()) << "ms" << std::endl;
				});
			}

			for (auto& thread : threads) thread.join();
			std::cout << std::endl << "===== 测试结果 =====" << std::endl;

			// 获取 Summary 统计对象
			const LikesProgram::Metrics::Summary summary = timer.GetSummary();
			// Summary 统计说明：
			// 1. 计时次数 = 单线程测试 + 拷贝测试 + 多线程测试次数之和
			// 2. 总耗时 = 单线程测试 + 拷贝测试 + 多线程测试耗时之和
			// 3. 最大耗时 = 单线程测试 + 拷贝测试 + 多线程测试耗时最大值
			// 原因：拷贝 Timer 会与源计时器共享 Summary，父子关系 Timer 也共享 Summary
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