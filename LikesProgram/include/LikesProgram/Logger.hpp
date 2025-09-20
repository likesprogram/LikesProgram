#pragma once
#include "LikesProgramLibExport.hpp"
#include <string>
#include <memory>
#include "String.hpp"
#include <thread>

namespace LikesProgram {
    class LIKESPROGRAM_API Logger {
    public:
        enum class LogLevel {
            Trace = 0,  // ����ϸ����־, �������ٳ����ϸ������Ϊ
            Debug,      // ��ϸ��־, ������¼�������������õ���Ϣ
            Info,       // ����ʱ��Ϣ, ����������������״̬
            Warn,       // ������Ϣ, ���������ܷ����Ĵ���
            Error,      // ������Ϣ, ������������⣬ĳЩ���ܿ���ʧЧ
            Fatal       // ����������Ϣ, ��������޷���������
        };

        struct LogMessage {
            LogLevel level = LogLevel::Trace;
            String msg;
            String file;
            int line = 0;
            String func;
            std::thread::id tid;
            String threadName;
            std::chrono::system_clock::time_point timestamp;
        };

        // ILogSink ����ӿ�
        class ILogSink {
        public:
            virtual ~ILogSink() = default;

            // д��־�ӿڣ��ɾ�������ʵ�֣�
            virtual void Write(const LogMessage& message, LogLevel minLevel, String::Encoding encoding) = 0;
            // �����࿪��
        protected:
            // ������������ʽ����־���ݣ���������ã�
            String FormatLogMessage(const LogMessage& message, LogLevel minLevel);
            // ������������־����ת�ַ�������������ã�
            const String LevelToString(LogLevel lvl);
        };

        // ��ȡȫ��Ψһʵ��
        static Logger& Instance();

        // ����ȫ����־����
        // ���ڸü������־�������˵�
        void SetLevel(LogLevel level);

        // ������־�������
        void SetEncoding(String::Encoding encoding);

        // ���һ����־���Ŀ�꣨Sink��
        void AddSink(std::shared_ptr<ILogSink> sink);

        // ��¼һ����־������ FW_LOG_xxx ʹ�ã�
        void Log(LogLevel level, const String& msg,
            const char* file, int line, const char* func);

        // ֹͣ��־ϵͳ��������̨�̣߳�������Դ��
        void Shutdown();

    private:
        // ���� / ������˽�л�����֤������
        Logger();
        ~Logger();

        // ��ֹ�����͸�ֵ
        Logger(const Logger&) = delete;
        Logger& operator=(const Logger&) = delete;

        // ��־����ѭ������̨�߳�ִ�У�
        void ProcessLoop();

        // �ڲ�ʵ��ϸ�ڣ�PImpl ���أ�
        struct Impl;
        Impl* pImpl; // ָ�뷽ʽ���ؾ���ʵ�֣����ٱ�������
    };

    // ��������
    std::shared_ptr<Logger::ILogSink> LIKESPROGRAM_API CreateConsoleSink();
    std::shared_ptr<Logger::ILogSink> LIKESPROGRAM_API CreateFileSink(const String& filename);

    // ��ӿ�
#define LOG_TRACE(msg) LikesProgram::Logger::Instance().Log(LikesProgram::Logger::LogLevel::Trace, msg, __FILE__, __LINE__, __func__)
#define LOG_DEBUG(msg) LikesProgram::Logger::Instance().Log(LikesProgram::Logger::LogLevel::Debug, msg, __FILE__, __LINE__, __func__)
#define LOG_INFO(msg)  LikesProgram::Logger::Instance().Log(LikesProgram::Logger::LogLevel::Info,  msg, __FILE__, __LINE__, __func__)
#define LOG_WARN(msg)  LikesProgram::Logger::Instance().Log(LikesProgram::Logger::LogLevel::Warn,  msg, __FILE__, __LINE__, __func__)
#define LOG_ERROR(msg) LikesProgram::Logger::Instance().Log(LikesProgram::Logger::LogLevel::Error, msg, __FILE__, __LINE__, __func__)
#define LOG_FATAL(msg) LikesProgram::Logger::Instance().Log(LikesProgram::Logger::LogLevel::Fatal, msg, __FILE__, __LINE__, __func__)
}
