#pragma once
#include "Sink.hpp"

namespace LikesProgram {
	namespace Log {
        class FileSink : public Sink {
        public:
            // 构造
            explicit FileSink(const LikesProgram::String& path, const LikesProgram::String& filename, size_t maxFileSizeMB = 30);
            ~FileSink();

            // 重写输出函数
            void Write(const Message& message) override;

            // 构建工厂
            static std::shared_ptr<Sink> CreateSink(const LikesProgram::String& path, const LikesProgram::String& filename, size_t maxFileSizeMB = 30);
        private:
            class FileSinkImpl;
            FileSinkImpl* m_impl;
        };
	}
}