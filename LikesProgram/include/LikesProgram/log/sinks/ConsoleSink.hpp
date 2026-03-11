#pragma once
#include "Sink.hpp"

namespace LikesProgram {
	namespace Log {
        class LIKESPROGRAM_API ConsoleSink : public Sink {
        public:
            ConsoleSink();
            static std::shared_ptr<Sink> CreateSink();

            void Write(const Message& message) override;
        };
	}
}