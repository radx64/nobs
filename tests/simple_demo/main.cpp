#include <print>

int foo();
int bar();

int main(int argc, char* argv[])
{
    std::println("Hello World!");
    std::println("Result of foo is {}", foo());
    std::println("Result of bar is {}", bar());
    return 0;
}
