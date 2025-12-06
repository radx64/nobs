#include <print>

#include <lib1.hpp>
#include <lib2.hpp>

int main()
{
    lib1::foo();
    lib2::bar();
    std::println("Yay!");
    return 0;
}