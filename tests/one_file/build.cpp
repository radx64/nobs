#include "../../nobs.hpp"

int main(int argc, const char* argv[])
{
    using namespace nobs;
    enable_self_rebuild();
    enable_command_line_params(argc, argv);
    set_build_directory("./build_dir");

    auto& target = add_executable("one_file_app");
    add_target_source(target, "main.cpp");
    add_target_compile_flag(target, "--std=c++26");

    build_target(target);

    return 0;
}

