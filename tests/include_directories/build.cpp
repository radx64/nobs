#include "../../nobs.hpp"

int main(const int argc, const char* argv[])
{
    using namespace nobs;
    enable_command_line_params(argc, argv);
    enable_self_rebuild();
    set_build_directory("./build_dir");
    auto& demo = add_executable("demo");
    add_target_sources(demo, 
        {
            "main.cpp",
        });

    add_target_include_directories(demo,
        {
            "./lib1/includes",
            "./lib2/includes",
        });

    add_target_compile_flag(demo, "--std=c++23");
    build_target(demo);

    return 0;
}
