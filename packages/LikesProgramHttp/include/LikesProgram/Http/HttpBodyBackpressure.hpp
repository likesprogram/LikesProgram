#pragma once
#include <LikesProgram/Http/system/LikesProgramHttpExport.hpp>

namespace LikesProgram {
    namespace Http {
        // Non-owning notification boundary for transport adapters. The
        // adapter decides how to pause or resume its read/write operation.
        class HttpBodyBackpressureAdapter {
        public:
            virtual ~HttpBodyBackpressureAdapter() = default;

            virtual void Pause() noexcept = 0;
            virtual void Resume() noexcept = 0;
        };
    }
}
