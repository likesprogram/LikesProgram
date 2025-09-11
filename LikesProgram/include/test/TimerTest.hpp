#pragma once
#include <iostream>
#include <thread>
#include <vector>
#include "../LikesProgram/Timer.hpp"

namespace TimerTest {
	void WorkLoad(size_t id) {
		LikesProgram::Timer::Start(); // ��ʼ��ʱ

		// ģ���ʱ����
		std::this_thread::sleep_for(std::chrono::milliseconds(100 + id * 250));

		auto elapsed = LikesProgram::Timer::Stop(); // ֹͣ����ȡ��ʱ
        std::cout << "Thread ��" << id << "����" << LikesProgram::Timer::ToString(elapsed) << std::endl;
	}

    void Test() {
		std::cout << "===== ���߳�ʾ�� =====" << std::endl;
		{
            LikesProgram::Timer timer(true); // ���첢����
            std::this_thread::sleep_for(std::chrono::milliseconds(250));

			std::cout << "�Ƿ����У�" << LikesProgram::Timer::IsRunning() << std::endl;
			auto elapsed = LikesProgram::Timer::Stop();
            std::cout << "���̣߳�" << LikesProgram::Timer::ToString(elapsed) << std::endl;

			std::cout << "�Ƿ����У�" << LikesProgram::Timer::IsRunning() << std::endl;
			std::cout << "���һ�κ�ʱ��" << LikesProgram::Timer::ToString(LikesProgram::Timer::GetLastElapsed()) << std::endl;
		}

		std::cout << std::endl << "===== ���߳�ʾ�� =====" << std::endl;
		{
			const int threadCount = 4;
            std::vector<std::thread> threads;

			// ��������߳�
			for (size_t i = 0; i < threadCount; i++) {
				threads.emplace_back(WorkLoad, i);
			}

			for (auto& thread : threads) thread.join();

			std::cout << std::endl << "===== ���Խ�� =====" << std::endl;
            std::cout << "�߳���ʱ�䣺" << LikesProgram::Timer::ToString(LikesProgram::Timer::GetTotalElapsed()) << std::endl;
			std::cout << "��ʱ�䣺" << LikesProgram::Timer::ToString(LikesProgram::Timer::GetTotalGlobalElapsed()) << std::endl;
            std::cout << "�ʱ�䣺" << LikesProgram::Timer::ToString(LikesProgram::Timer::GetLongestElapsed()) << std::endl;
            std::cout << "EMAƽ��ʱ�䣺" << LikesProgram::Timer::ToString(LikesProgram::Timer::GetEMAAverageElapsed()) << std::endl;
			std::cout << "�߳�����ƽ��ʱ�䣺" << LikesProgram::Timer::ToString(LikesProgram::Timer::GetArithmeticAverageElapsed()) << std::endl;
			std::cout << "ȫ������ƽ��ʱ�䣺" << LikesProgram::Timer::ToString(LikesProgram::Timer::GetArithmeticAverageGlobalElapsed()) << std::endl;
		}

		std::cout << std::endl << "===== ����ʾ�� =====" << std::endl;
		{
			LikesProgram::Timer::ResetThread(); // ���õ�ǰ�̼߳�ʱ����
			LikesProgram::Timer::ResetGlobal(); // ����ȫ�ּ�ʱ����

			std::cout << "�߳���ʱ�䣺" << LikesProgram::Timer::ToString(LikesProgram::Timer::GetTotalElapsed()) << std::endl;
			std::cout << "��ʱ�䣺" << LikesProgram::Timer::ToString(LikesProgram::Timer::GetTotalGlobalElapsed()) << std::endl;
            std::cout << "�ʱ�䣺" << LikesProgram::Timer::ToString(LikesProgram::Timer::GetLongestElapsed()) << std::endl;
			std::cout << "EMAƽ��ʱ�䣺" << LikesProgram::Timer::ToString(LikesProgram::Timer::GetEMAAverageElapsed()) << std::endl;
			std::cout << "�߳�����ƽ��ʱ�䣺" << LikesProgram::Timer::ToString(LikesProgram::Timer::GetArithmeticAverageElapsed()) << std::endl;
			std::cout << "ȫ������ƽ��ʱ�䣺" << LikesProgram::Timer::ToString(LikesProgram::Timer::GetArithmeticAverageGlobalElapsed()) << std::endl;
		}
    }
}