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
            "foo.cpp",
            "subdir/bar.cpp"
            
        });

    add_target_compile_flag(demo, "-std=c++23");
    build_target(demo);
    
    set_build_directory("./build_dir");
    auto& demo2 = add_executable("demo2");
    add_target_sources(demo2, 
        {
            current_project_directory() + "/main.cpp",
            current_project_directory() + "/foo2.cpp",
            current_project_directory() + "/subdir2/bar.cpp"
        });

    add_target_compile_flag(demo2, "-std=c++23");
    build_target(demo2);
    return 0;
}
