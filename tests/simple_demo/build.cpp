#include "nobs.hpp"

int main(const int argc, const char* argv[])
{
    using namespace nobs;
    enable_command_line_params(argc, argv);
    enable_self_rebuild();
    set_build_directory("./build_dir");
    add_executable("demo");
    add_target_sources("demo", 
        {
            "main.cpp",
            "foo.cpp",
            "subdir/bar.cpp"
        });

    add_target_compile_flag("demo", "-std=c++23");
    build_target("demo");

    add_executable("demo2");
    add_target_sources("demo2", 
        {
            "main.cpp",
            "foo2.cpp",
            "subdir2/bar.cpp"
        });

    add_target_compile_flag("demo2", "-std=c++23");
    build_target("demo2");

    //DEBUG_list();
}
