#include <iostream>
#include "../include/test/TimerTest.hpp"
#include "../include/test/VectorTest .hpp"
#include "../include/test/StringTest.hpp"
#include "../include/test/UnicodeTest.hpp"

int main()
{
    std::cout << "===== TimerTest =====" << std::endl << std::endl;
    TimerTest::Test();

    std::cout << std::endl << std::endl << "===== VectorTest =====" << std::endl << std::endl;
    VectorTest::Test();

    std::cout << std::endl << std::endl << "===== StringTest =====" << std::endl << std::endl;
    //StringTest::Test();

    std::cout << std::endl << std::endl << "===== UnicodeTest =====" << std::endl << std::endl;
    UnicodeTest::Test();
    return 0;
}