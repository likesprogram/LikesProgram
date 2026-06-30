#include <LikesProgram/Core/String.hpp>
#include <LikesProgram/Core/Deadline.hpp>
#include <LikesProgram/Core/Platform.hpp>
#include <LikesProgram/Core/Result.hpp>
#include <LikesProgram/Core/Version.hpp>
#include <LikesProgram/Core/time/Time.hpp>
#include <LikesProgram/Core/time/Timer.hpp>
#include <chrono>
#include <iostream>
#include <thread>

// Core 示例展示 String::Format、Time::FormatTime 和 Timer 的最小组合用法。
int main() {
    LikesProgram::String name(u8"LikesProgramCore"); // 示例组件名
    LikesProgram::String message = LikesProgram::String::Format(u"{} {} ready at {}", name,
        LikesProgram::String(LikesProgram::Version::CurrentString()),
        LikesProgram::Time::FormatTime(std::chrono::system_clock::now(), u"%Y-%m-%d %H:%M:%S"));

    std::cout << message.ToStdString() << std::endl;
    std::cout << LikesProgram::String::Format(u"platform: {} {} {}",
        LikesProgram::Platform::OperatingSystemName().data(),
        LikesProgram::Platform::ArchitectureName().data(),
        LikesProgram::Platform::CompilerName().data()).ToStdString() << std::endl;

    LikesProgram::Time::Timer timer(true); // 自动启动的示例计时器
    auto deadline = LikesProgram::Time::Deadline::FromNow(std::chrono::milliseconds(100)); // 示例截止时间
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    auto elapsed = timer.Stop(); // 示例任务耗时

    std::cout << LikesProgram::String::Format(u"elapsed: {} ns", elapsed.count()).ToStdString() << std::endl;
    std::cout << LikesProgram::String::Format(u"deadline remaining: {} ns",
        deadline.Remaining().count()).ToStdString() << std::endl;

    LikesProgram::Result<LikesProgram::String> result(LikesProgram::String(U"ok")); // 示例状态返回值
    if (result) {
        std::cout << LikesProgram::String::Format(u"result: {}", result.Value()).ToStdString() << std::endl;
    }

    return 0;
}
