#include "nobs.hpp"

int main()
{
    using namespace nobs;
    set_build_directory("./build_dir");
    add_executable("demo");
    add_target_sources("demo", 
        {
            "main.cpp",
            "foo.cpp"
        });

    add_target_compile_flag("demo", "-std=c++23");
            
    //add_target_source("error", "main.cpp");

    DEBUG_list();

    build_target("demo");
}
