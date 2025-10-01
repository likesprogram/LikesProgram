#include <iostream>
#include "../include/test/PercentileSketchTest.hpp"
#include "../include/test/VectorTest.hpp"
#include "../include/test/Vector3Test.hpp"
#include "../include/test/Vector4Test.hpp"
#include "../include/test/TimerTest.hpp"
#include "../include/test/UnicodeTest.hpp"
#include "../include/test/StringTest.hpp"
#include "../include/test/MetricsTest.hpp"
#include "../include/LikesProgram/CoreUtils.hpp"
#include "../include/test/LoggerTest.hpp"
#include "../include/test/ThreadPoolTest.hpp"
#include "../include/test/ConfigurationTest.hpp"
#include "../include/test/Test.hpp"

int main()
{
    uint64_t i = 0;

	do {
        std::cout << std::dec << std::endl << "===== Test【" << (i) <<"】 =====" << std::endl << std::endl;
        LikesProgram::String uuid = LikesProgram::CoreUtils::GenerateUUID(LikesProgram::String(""));
        std::cout << "UUID: " << uuid << std::endl;

        std::cout << std::endl << std::endl << "===== VectorTest =====" << std::endl << std::endl;
        VectorTest::Test();

        std::cout << std::endl << std::endl << "===== Vector3Test =====" << std::endl << std::endl;
        Vector3Test::Test();

        std::cout << std::endl << std::endl << "===== Vector4Test =====" << std::endl << std::endl;
        Vector4Test::Test();

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
    } while (true);
    return 0;
}