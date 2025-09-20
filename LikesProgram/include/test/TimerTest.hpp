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
		LikesProgram::Timer threadTimer(true, data->timer); // �����̼߳�ʱ��

		// ģ���ʱ����
		std::this_thread::sleep_for(std::chrono::milliseconds(100 + data->index * 20));

		auto elapsed = threadTimer.Stop(); // ֹͣ����ȡ��ʱ
        std::cout << "Thread ��" << data->index << "����" << LikesProgram::Timer::ToString(elapsed) << std::endl;
	}

    void Test() {
		LikesProgram::Timer timer; // ȫ�ּ�ʱ��

		std::cout << "===== ���߳�ʾ�� =====" << std::endl;
		{
			timer.Start(); // ��ʼ��ʱ
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

			//std::cout << "�Ƿ����У�" << timer.IsRunning() << std::endl;
			auto elapsed = timer.Stop();
            std::cout << "���̣߳�" << LikesProgram::Timer::ToString(elapsed) << std::endl;

			std::cout << "�Ƿ����У�" << timer.IsRunning() << std::endl;
			std::cout << "���һ�κ�ʱ��" << LikesProgram::Timer::ToString(timer.GetLastElapsed()) << std::endl;
		}

		std::cout << std::endl << "===== ���߳�ʾ�� =====" << std::endl;
		{
			const int threadCount = 4;
            std::vector<std::thread> threads;

			// ��������߳�
			for (size_t i = 0; i < threadCount; i++) {
				ThreadData* data = new ThreadData();
				data->index = i;
                data->timer = &timer;
				threads.emplace_back(WorkLoad, data);
			}

			for (auto& thread : threads) thread.join();
			std::cout << std::endl << "===== ���Խ�� =====" << std::endl;
			std::cout << "�߳���ʱ�䣺" << LikesProgram::Timer::ToString(timer.GetTotalElapsed()) << std::endl;
			std::cout << "�ʱ�䣺" << LikesProgram::Timer::ToString(timer.GetLongestElapsed()) << std::endl;
			std::cout << "EMAƽ��ʱ�䣺" << LikesProgram::Timer::ToString(timer.GetEMAAverageElapsed()) << std::endl;
			std::cout << "�߳�����ƽ��ʱ�䣺" << LikesProgram::Timer::ToString(timer.GetArithmeticAverageElapsed()) << std::endl;
		}

		std::cout << std::endl << "===== ����ʾ�� =====" << std::endl;
		{
			timer.Reset();

			std::cout << "�߳���ʱ�䣺" << LikesProgram::Timer::ToString(timer.GetTotalElapsed()) << std::endl;
			std::cout << "�ʱ�䣺" << LikesProgram::Timer::ToString(timer.GetLongestElapsed()) << std::endl;
			std::cout << "EMAƽ��ʱ�䣺" << LikesProgram::Timer::ToString(timer.GetEMAAverageElapsed()) << std::endl;
			std::cout << "�߳�����ƽ��ʱ�䣺" << LikesProgram::Timer::ToString(timer.GetArithmeticAverageElapsed()) << std::endl;
		}
    }
}