#pragma once
#include "../LikesProgram/ThreadPool.hpp"
#include "../LikesProgram/Logger.hpp"
#include "../LikesProgram/String.hpp"

namespace ThreadPoolTest {
	void Test() {        // 初始化日志
        auto& logger = LikesProgram::Logger::Instance();
#ifdef _WIN32
        logger.SetEncoding(LikesProgram::String::Encoding::GBK);
#endif
        logger.SetLevel(LikesProgram::Logger::LogLevel::Trace);
        // 内置控制台输出 Sink
        logger.AddSink(LikesProgram::Logger::CreateConsoleSink()); // 输出到控制台

        LikesProgram::ThreadPool::Options optins = {
        2,   // 最小线程数
        4,   // 线程数上限
        2048,// 队列长度限制
        LikesProgram::ThreadPool::RejectPolicy::Block, // 任务拒绝策略
        std::chrono::milliseconds(100), // 空闲线程回收时间
        true, // 是否启用动态扩容、缩容
        };

        // 创建线程池
        // LikesProgram::ThreadPool pool(optins); // 使用自定义参数创建线程池
        LikesProgram::ThreadPool pool; // 使用默认参数创建线程池
        pool.Start();

        // 提交一些任务
        for (int i = 0; i < 30; i++) {
            // 提交无返回值无参数的任务
            pool.PostNoArg([i]() {
                LOG_DEBUG(u"PostNoArg：Hello from worker");
            });

            // 提交无返回值有参数的任务
            pool.Post([](LikesProgram::String message) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                LikesProgram::String out;
                out.Append(message);
                out.Append(u"：Hello from worker");
                LOG_DEBUG(out);
            }, u"Post");

            // 提交有返回值有参数的任务
            auto poolOut = pool.Submit([i](LikesProgram::String message) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                LOG_DEBUG(message);
                LikesProgram::String out;
                out.Append(message);
                out.Append(u"：[");
                out.Append(LikesProgram::String(std::to_string(i)));
                out.Append(u"] Out");
                return out;
            }, u"Submit");

            // 等待任务完成, 并获取结果（这里会拖慢任务效率）
            // LOG_WARN(poolOut.get());

            if (i % 10 == 0) {
                LOG_WARN(poolOut.get()); // 每隔10次输出一次 Submit 的运行结果

                // 获取快照统计信息
                LikesProgram::ThreadPool::Statistics stats = pool.Snapshot();
                LOG_WARN(stats.ToString());
            }
        }

        // 关闭线程池
        pool.Shutdown();
        if (pool.AwaitTermination(std::chrono::milliseconds(1000))) { // 等待线程池关闭
            LOG_WARN(u"线程池已关闭");
        } else {
            LOG_ERROR(u"线程池关闭超时");
        }

        // 获取快照统计信息
        LikesProgram::ThreadPool::Statistics stats = pool.Snapshot();
        LOG_WARN(stats.ToString());

        std::this_thread::sleep_for(std::chrono::seconds(1)); // 给后台线程一点时间输出
        logger.Shutdown();
	}
}