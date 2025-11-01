#include <print>

int foo();

int main(int argc, char* argv[])
{
    std::println("Hello World!");
    std::println("Result of foo is {}", foo());
    return 0;
}
