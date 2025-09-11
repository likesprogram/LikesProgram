#include <iostream>
#include "../include/test/TimerTest.hpp"
#include "../include/test/VectorTest .hpp"

int main()
{
    std::cout << "===== TimerTest =====" << std::endl << std::endl;
    TimerTest::Test();

    std::cout << std::endl << std::endl << "===== VectorTest =====" << std::endl << std::endl;
    VectorTest::Test();
    return 0;
}