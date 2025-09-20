#pragma once
#include "../LikesProgram/ThreadPool.hpp"
#include "../LikesProgram/Logger.hpp"
#include "../LikesProgram/String.hpp"

namespace ThreadPoolTest {
	void Test() {        // ��ʼ����־
        auto& logger = LikesProgram::Logger::Instance();
#ifdef _WIN32
        logger.SetEncoding(LikesProgram::String::Encoding::GBK);
#endif
        logger.SetLevel(LikesProgram::Logger::LogLevel::Trace);
        // ���ÿ���̨��� Sink
        logger.AddSink(LikesProgram::CreateConsoleSink()); // ���������̨

        LikesProgram::ThreadPool::Options optins = {
        2,   // ��С�߳���
        4,   // �߳�������
        2048,// ���г�������
        LikesProgram::ThreadPool::RejectPolicy::Block, // ����ܾ�����
        std::chrono::milliseconds(100), // �����̻߳���ʱ��
        true, // �Ƿ����ö�̬���ݡ�����
        };

        // �����̳߳�
        // LikesProgram::ThreadPool pool(optins); // ʹ���Զ�����������̳߳�
        LikesProgram::ThreadPool pool; // ʹ��Ĭ�ϲ��������̳߳�
        pool.Start();

        // �ύһЩ����
        for (int i = 0; i < 30; i++) {
            // �ύ�޷���ֵ�޲���������
            pool.PostNoArg([i]() {
                LOG_DEBUG(u"PostNoArg��Hello from worker");
            });

            // �ύ�޷���ֵ�в���������
            pool.Post([](LikesProgram::String message) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                LikesProgram::String out;
                out.Append(message);
                out.Append(u"��Hello from worker");
                LOG_DEBUG(out);
            }, u"Post");

            // �ύ�з���ֵ�в���������
            auto poolOut = pool.Submit([i](LikesProgram::String message) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                LOG_DEBUG(message);
                LikesProgram::String out;
                out.Append(message);
                out.Append(u"��[");
                out.Append(LikesProgram::String(std::to_string(i)));
                out.Append(u"] Out");
                return out;
            }, u"Submit");

            // �ȴ��������, ����ȡ������������������Ч�ʣ�
            // LOG_WARN(poolOut.get());

            if (i % 10 == 0) {
                LOG_WARN(poolOut.get()); // ÿ��10�����һ�� Submit �����н��

                // ��ȡ����ͳ����Ϣ
                LikesProgram::ThreadPool::Statistics stats = pool.Snapshot();
                LOG_WARN(stats.ToString());
            }
        }

        // �ر��̳߳�
        pool.Shutdown();
        if (pool.AwaitTermination(std::chrono::milliseconds(1000))) { // �ȴ��̳߳عر�
            LOG_WARN(u"�̳߳��ѹر�");
        } else {
            LOG_ERROR(u"�̳߳عرճ�ʱ");
        }

        // ��ȡ����ͳ����Ϣ
        LikesProgram::ThreadPool::Statistics stats = pool.Snapshot();
        LOG_WARN(stats.ToString());

        std::this_thread::sleep_for(std::chrono::seconds(1)); // ����̨�߳�һ��ʱ�����
        logger.Shutdown();
	}
}