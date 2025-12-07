#include "../../nobs.hpp"

int main(const int argc, const char* argv[])
{
    using namespace nobs;
    enable_command_line_params(argc, argv);
    enable_self_rebuild();
    set_build_directory("./build_dir");

    auto& staticlibrary = add_library("some_crazy_lib");
    add_target_sources(staticlibrary,
        {
            "lib.cpp"
        });

    auto& anotherstaticlibrary = add_library("some_other_crazy_lib");
    add_target_sources(anotherstaticlibrary,
        {
            "other_lib.cpp"
        });
        

    auto& demo = add_executable("demo");
    add_target_sources(demo, 
        {
            "main.cpp",
        });

    add_target_compile_flag(demo, "-std=c++23");
    target_link_libraries(demo, 
        {
            staticlibrary,
            anotherstaticlibrary
        });

    build_target(demo);

    return 0;
}
