#include "nobs.hpp"

int main()
{
    using namespace nobs;
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

    DEBUG_list();
}
