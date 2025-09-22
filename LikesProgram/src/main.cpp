#include <iostream>
#include "../include/test/TimerTest.hpp"
#include "../include/test/VectorTest .hpp"
#include "../include/test/StringTest.hpp"
#include "../include/test/UnicodeTest.hpp"
#include "../include/LikesProgram/CoreUtils.hpp"
#include "../include/test/LoggerTest.hpp"
#include "../include/test/ThreadPoolTest.hpp"
#include "../include/test/ConfigurationTest.hpp"

int main()
{
	do {
        LikesProgram::String uuid = LikesProgram::CoreUtils::GenerateUUID(LikesProgram::String(""));
        std::cout << "UUID: " << uuid << std::endl;
        std::cout << "===== TimerTest =====" << std::endl << std::endl;
        TimerTest::Test();

        std::cout << std::endl << std::endl << "===== VectorTest =====" << std::endl << std::endl;
        VectorTest::Test();

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
    } while (true);
    return 0;
}