#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "../../../include/LikesProgram/log/Logger.hpp"
#include "../../../include/LikesProgram/system/CoreUtils.hpp"
#include <iostream>
#include <mutex>
#include <shared_mutex>
#include <chrono>
#include <thread>
#include <condition_variable>
#include <queue>
#include <atomic>

namespace LikesProgram {
    namespace Log {
        struct Logger::LoggerImpl {
            std::atomic<bool> stop{ false };
            Level minLevel = Level::Trace;

            std::shared_mutex sinkMtx;
            std::vector<std::shared_ptr<Sink>> sinks;

            std::mutex queueMtx;
            std::mutex startMutex;
            std::condition_variable cv;
            std::queue<Message> queue;
            std::thread worker;
            String::Encoding encoding = String::Encoding::UTF8;

            bool m_debug = false;
        };

        Logger& Logger::Instance() {
            return Instance(false, false);
        }

        Logger& Logger::Instance(bool autoStart) {
            return Instance(autoStart, false);
        }

        // Logger API
        Logger& Logger::Instance(bool autoStart, bool debug) {
            static std::atomic<Logger*> instance{ nullptr };
            static std::mutex mutex;

            // 获取实例，若没有则创建
            Logger* inst = instance.load(std::memory_order_acquire);
            if (!inst) {
                std::lock_guard lock(mutex);
                inst = instance.load(std::memory_order_relaxed);
                if (!inst) {
                    inst = new Logger(autoStart, debug);
                    instance.store(inst, std::memory_order_release);
                    //CoreUtils::SetCurrentThreadName(u"主线程"); // 默认将初始化线程名为“主线程”
                }
            }

            // 启动控制
            if (inst->m_impl->stop.load(std::memory_order_acquire) && autoStart) inst->Start();

            return *inst;
        }

        Logger::Logger(bool autoStart, bool debug) {
            m_impl = new LoggerImpl{};
            m_impl->m_debug = debug;
            m_impl->stop.store(true, std::memory_order_release); // 第一时间打 stop 标记
            if (autoStart) Start();
        }

        Logger::~Logger() {
            if (m_impl) {
                m_impl->stop.store(true, std::memory_order_release); // 第一时间打 stop 标记
                m_impl->cv.notify_all(); // 唤醒阻塞的 worker

                if (m_impl->worker.joinable()) m_impl->worker.join(); // 等待线程退出

                while (!m_impl->queue.empty()) m_impl->queue.pop();
                m_impl->sinks.clear();

                delete m_impl;
                m_impl = nullptr;
            }
        }

        void Logger::ProcessLoop() {
            while (!m_impl->stop) {
                std::unique_lock<std::mutex> lock(m_impl->queueMtx);
                m_impl->cv.wait(lock, [this]() { return !m_impl->queue.empty() || m_impl->stop; });

                if (m_impl->stop && m_impl->queue.empty()) break;

                while (!m_impl->queue.empty()) {
                    auto msg = m_impl->queue.front();
                    m_impl->queue.pop();
                    lock.unlock();

                    // 多个 sink 并发读
                    {
                        std::shared_lock sinkLock(m_impl->sinkMtx); // 共享锁
                        for (auto& sink : m_impl->sinks) sink->Write(msg);
                    }

                    lock.lock();
                }
            }
        }

        void Logger::SetLevel(Level level) {
            m_impl->minLevel = level;
        }

        void Logger::SetEncoding(String::Encoding encoding) {
            m_impl->encoding = encoding;
        }

        void Logger::AddSink(std::shared_ptr<Sink> sink) {
            std::unique_lock lock(m_impl->sinkMtx); // 独占锁
            m_impl->sinks.push_back(sink);
        }

        bool Logger::Start() {
            bool expected = true;  // true 表示“停止状态”
            // CAS 检查并更新为 false（启动状态）
            if (!m_impl->stop.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
                // compare_exchange_strong 失败说明已经是 false（即已启动）
                return true;
            }

            std::lock_guard<std::mutex> lock(m_impl->startMutex); // 保护线程创建
            if (!m_impl->worker.joinable()) {
                try {
                    m_impl->worker = std::thread([this]() { ProcessLoop(); });
                } catch (...) {
                    m_impl->stop.store(true, std::memory_order_release); // 启动失败恢复状态
                    throw;
                }
            }
            // 返回 线程 joinable 状态
            return m_impl->worker.joinable();
        }

        void Logger::Shutdown(bool clearSink) {
            if (!m_impl) return;

            // 原子检查：只允许一次关闭
            bool expected = false;
            if (!m_impl->stop.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
                // stop 已经是 true，说明已经关闭过
                return;
            }

            // 通知所有等待线程
            m_impl->cv.notify_all();

            // 等待 worker 线程退出
            if (m_impl->worker.joinable()) {
                try {
                    m_impl->worker.join();
                }
                catch (...) {
                    // 极端情况下 swallow 异常，保证 Shutdown 不抛出
                }
            }

            if (clearSink) {
                std::unique_lock lock(m_impl->sinkMtx);
                m_impl->sinks.clear();
            }
        }

        void Logger::LogMessageString(Level level, const String& msg, const char* file, int line, const char* func) {
            if (!m_impl) return;
            if (m_impl->stop.load(std::memory_order_acquire)) return;
            if (level < m_impl->minLevel) return;

            try {
                Message message;
                message.level = level;
                message.msg = String(msg);
                message.file = (file != nullptr) ? String(file) : String();
                message.line = line;
                message.tid = std::this_thread::get_id();
                message.threadName = CoreUtils::GetCurrentThreadName();
                message.timestamp = std::chrono::system_clock::now();
                message.func = (func != nullptr) ? String(func) : String();
                message.debug = m_impl->m_debug;
                message.minLevel = m_impl->minLevel;
                message.encoding = m_impl->encoding;
                // 使用 queueMtx 同步 stop 和 queue push
                {
                    std::lock_guard<std::mutex> lock(m_impl->queueMtx);
                    if (m_impl->stop.load(std::memory_order_acquire)) return;
                    m_impl->queue.push(std::move(message));
                }
                m_impl->cv.notify_one();
            }
            catch (const std::exception& e) {
                std::cerr << "[Logger Error] Failed to log message: " << e.what() << std::endl;
            }
            catch (...) {
                std::cerr << "[Logger Error] Failed to log message: Unknown exception" << std::endl;
            }
        }
    }
}
