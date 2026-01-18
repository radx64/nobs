#include <print>

int lib_call();
int other_lib_call();

int main(int argc, char* argv[])
{
    std::println("Lib returned {}", lib_call());
    std::println("Otherlib returned {}", other_lib_call());
    return 0;
}
