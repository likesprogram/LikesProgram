#include <iostream>
#include "../include/test/PercentileSketchTest.hpp"
#include "../include/test/VectorTest .hpp"
#include "../include/test/TimerTest.hpp"
#include "../include/test/UnicodeTest.hpp"
#include "../include/test/StringTest.hpp"
#include "../include/test/MetricsTest.hpp"
#include "../include/LikesProgram/CoreUtils.hpp"
#include "../include/test/LoggerTest.hpp"
#include "../include/test/ThreadPoolTest.hpp"
#include "../include/test/ConfigurationTest.hpp"
#include "../include/test/Test.hpp"
#include <windows.h>
#include <psapi.h>
#include <chrono>
#include <fstream>

// link psapi
#pragma comment(lib, "psapi.lib")

// 返回进程 Private Bytes（字节）
static SIZE_T GetPrivateBytes()
{
    PROCESS_MEMORY_COUNTERS_EX pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
        return pmc.PrivateUsage; // bytes
    }
    return 0;
}

// 可选：把 bytes 转为人类可读字符串（调试用）
static std::string HumanReadable(SIZE_T bytes) {
    const char* units[] = { "B", "KB", "MB", "GB" };
    double b = static_cast<double>(bytes);
    int i = 0;
    while (b >= 1024.0 && i < 3) { b /= 1024.0; ++i; }
    char buf[64];
    sprintf_s(buf, "%.2f %s", b, units[i]);
    return std::string(buf);
}

int main()
{
    uint64_t i = 0;

    // 打开日志文件（追加），放在可写目录
    std::ofstream memlog("memlog.csv", std::ios::out | std::ios::trunc);
    if (!memlog.is_open()) {
        std::cerr << "无法打开 memlog.csv 写入\n";
        return 1;
    }
    // 写入 CSV 表头
    memlog << "iteration,timestamp_ms,private_bytes\n";
    memlog.flush();

	do {
        std::cout << std::dec << std::endl << "===== Test【" << (i) <<"】 =====" << std::endl << std::endl;
        LikesProgram::String uuid = LikesProgram::CoreUtils::GenerateUUID(LikesProgram::String(""));
        std::cout << "UUID: " << uuid << std::endl;

        std::cout << std::endl << std::endl << "===== VectorTest =====" << std::endl << std::endl;
        VectorTest::Test();

        std::cout << "===== TimerTest =====" << std::endl << std::endl;
        TimerTest::Test();

        std::cout << std::endl << std::endl << "===== PercentileSketchTest =====" << std::endl << std::endl;
        PercentileSketchTest::Test();

        std::cout << std::endl << std::endl << "===== MetricsTest =====" << std::endl << std::endl;
        MetricsTest::Test();

        std::cout << std::endl << std::endl << "===== UnicodeTest =====" << std::endl << std::endl;
        UnicodeTest::Test();

        std::cout << std::endl << std::endl << "===== StringTest =====" << std::endl << std::endl;
        StringTest::Test();

        std::cout << std::endl << std::endl << "===== LoggerTest =====" << std::endl << std::endl;
        LoggerTest::Test();

        std::cout << std::endl << std::endl << "===== ThreadPoolTest =====" << std::endl << std::endl;
        ThreadPoolTest::Test();

        std::cout << std::endl << std::endl << "===== ConfigurationTest =====" << std::endl << std::endl;
        ConfigurationTest::Test();

        i++;
        // 每 1000 次记录一次（根据需要改为 500/10000）
        if ((i % 1000) == 0) {
            // 时间戳：自 epoch 的毫秒数
            auto now = std::chrono::system_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

            SIZE_T pb = GetPrivateBytes(); // bytes

            // CSV: iteration,timestamp_ms,private_bytes
            memlog << i << ',' << ms << ',' << pb << '\n';
            memlog.flush(); // 保证立即写入磁盘，便于外部查看和崩溃后仍保留数据

            // 可选：调试输出（只在你想看的时候开启）
            std::cout << "iter=" << i << " pb=" << pb << " (" << HumanReadable(pb) << ")\n";
        }
    } while (true);

    memlog.close();
    return 0;
}