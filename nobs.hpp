#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <print>
#include <ranges>
#include <source_location>
#include <string_view>
#include <string>
#include <vector>
#include <unistd.h>

namespace nobs
{
    auto constexpr compiler = "g++";
    auto constexpr linker = "g++";

struct Target
{
    std::string name;
    std::vector<std::filesystem::path> sources;
    std::vector<std::string> compile_flags;
    bool needs_linking {false};
};

struct Meta
{
    uint64_t timestamp;
    std::string compile_flags;

    void operator=(const Meta& rhs);
};

bool operator==(const Meta& lhs, const Meta& rhs)
{
    return (lhs.timestamp == rhs.timestamp) 
        and (lhs.compile_flags == rhs.compile_flags);
}

void Meta::operator=(const Meta& rhs)
{
    timestamp = rhs.timestamp;
    compile_flags = rhs.compile_flags;
}

static std::vector<Target> targets {};
static std::filesystem::path build_directory {"./build_dir"};  // build in "build_dir" by default
static std::filesystem::path project_directory {std::filesystem::current_path()};

void set_project_directory(const std::string_view& project_dir)
{
    project_directory = std::filesystem::path(project_dir);
}

void set_build_directory(const std::string_view& build_dir)
{
    build_directory = std::string(build_dir);
}

void trace_error(const std::string_view& error_string, const std::source_location location = std::source_location::current())
{
    std::println("Error at {}:{}: {}", location.file_name(), location.line(), error_string);
}

void add_executable(const std::string_view& name)
{
    targets.emplace_back(std::string(name));
}

void create_directory_if_missing(const std::filesystem::path directory)
{
    try
    { 
        auto result = std::filesystem::create_directories(directory);
    }
    catch (std::filesystem::filesystem_error& error)
    {
        trace_error(std::format("ERR: got {} - code {}", error.what(), error.code().message()));
        throw;
    }
}

auto& get_target(const std::string_view& target, const std::source_location location = std::source_location::current())
{
    auto view = targets 
        | std::views::filter([&](Target& t) {return t.name == target;})
        | std::views::take(1);

    if (view.empty())
    {
        trace_error(std::format("No target with name \"{}\" found! Exiting!", target), location);
        exit(-1);
    }

    return view.front();
}

void add_target_sources(const std::string_view& target, 
    const std::vector<std::string_view>& sources, 
    const std::source_location location = std::source_location::current())
{
    auto& found = get_target(target, location);

    for (const auto& source : sources)
    {
        found.sources.push_back(std::filesystem::path(source));
    }
}

void add_target_source(const std::string_view& target, 
    const std::string_view& source, 
    const std::source_location location = std::source_location::current())
{
    add_target_sources(target, {source}, location);
}

void add_target_compile_flags(const std::string_view& target,
    const std::vector<std::string_view>& flags,
    const std::source_location location = std::source_location::current())
{
    auto& found = get_target(target, location);

    for (const auto& flag : flags)
    {
        found.compile_flags.push_back(std::string(flag));
    }    
}

void add_target_compile_flag(const std::string_view& target,
    const std::string_view& flag,
    const std::source_location location = std::source_location::current())
{
    add_target_compile_flags(target, {flag}, location);
}

Meta prepare_new_meta_for_file(const std::filesystem::path& source_file, const std::string& flags)
{
    Meta meta{};
    auto source_timestamp = std::filesystem::last_write_time(source_file);
    meta.timestamp = static_cast<uint64_t>(source_timestamp.time_since_epoch().count());
    meta.compile_flags = flags;
    return meta;
}

Meta read_meta_info_from_file(const std::filesystem::path& metafile)
{
   Meta meta{};
   std::ifstream file{metafile.c_str()};

   if (not file)
   {
        trace_error("Error opening file!");
        exit(1);
   }

    std::string line{};

    if (not std::getline(file, line)) trace_error(std::format("Could not read timestamp from metafile {}", metafile.string()));
    meta.timestamp = std::stoull(line.c_str());

    if (not std::getline(file, line)) trace_error(std::format("Could not read compiler flags from metafile {}", metafile.string()));
    meta.compile_flags = line.c_str();

    return meta;
}

auto get_file_metafile_name(const std::filesystem::path& source_file, const std::filesystem::path& build_source_path)
{
    auto meta_file = std::filesystem::canonical(build_source_path);
    meta_file /= source_file.filename();
    meta_file += ".meta";
    return meta_file;
}

void make_meta_file(const std::filesystem::path& source_file, const std::filesystem::path& build_source_path, const Meta& meta_info)
{
    auto meta_file = get_file_metafile_name(source_file, build_source_path);

    if (std::ofstream file{meta_file.c_str()}; file) {
        std::println(file, "{}", meta_info.timestamp);
        std::println(file, "{}", meta_info.compile_flags);
    } else {
        trace_error("Error opening file!");
        exit(-1);
    } 
}

void compile_file(Target& target, const std::string& flags, const bool use_build_dir, const std::filesystem::path& source)
{
    auto build_source_file = build_directory / source;
    auto build_source_path = build_source_file.parent_path();

    if (use_build_dir)  
    {
        create_directory_if_missing(build_source_path);
    }
    else   
    {
        build_source_file = source;
        build_source_path = ".";
    }

    auto object_file = std::filesystem::canonical(build_source_path);
    object_file /= source.filename();
    object_file += ".o";

    auto meta_file = get_file_metafile_name(source, build_source_path);

    auto new_file_meta_info = prepare_new_meta_for_file(source, flags);

    if (std::filesystem::exists(meta_file))
    {
        auto old = read_meta_info_from_file(meta_file);

        if (old == new_file_meta_info)
        {
            std::println("Source not changed. No need to build {}", std::filesystem::canonical(source).string());
            return;
        }
    }

    target.needs_linking = true;
    
    auto compile_parameters = std::format("-c -o {0} {1}", object_file.string(), 
        std::filesystem::canonical(source).string());

    auto job = std::format("{} {}{}", compiler, flags, compile_parameters);
    std::println("{}", job);
    std::system(job.c_str());  // todo: replace system with something more sophisticated (to let parallel execution)
                                // also stop on fail need to be implemented

    make_meta_file(source, build_source_path, new_file_meta_info);
}

void compile_target(Target& target, const bool use_build_dir = true, const std::source_location location = std::source_location::current())
{
    create_directory_if_missing(build_directory);
    
    std::string flags{};
    for (const auto & flag : target.compile_flags)
    {  
        flags.append(std::format("{} ", flag));
    }

    for (const auto& source : target.sources)
    {    
        compile_file(target, flags, use_build_dir, source);
    }
}

void link_target(const Target& target, const bool use_build_dir = true, const std::source_location location = std::source_location::current())
{
    if (not target.needs_linking)
    {
        std::println("No need to relink {}", target.name);
        return;
    }

    auto canonical_build_dir = std::filesystem::canonical(build_directory);
    if (not use_build_dir) canonical_build_dir = std::filesystem::canonical(".");

    auto link_parameters = std::format("-o {}", (canonical_build_dir / target.name).string());

    for (const auto& source : target.sources)
    {
        auto build_source_object_file = (canonical_build_dir / source) += ".o";
        link_parameters.append(std::string(" ") + build_source_object_file.string());
    }

    auto job = std::format("{} {}", linker, link_parameters);
    std::println("{}", job);
    std::system(job.c_str());  // todo: replace system with something more sophisticated (to let parallel execution)
}

void build_target(const std::string_view& target, const std::source_location location = std::source_location::current())
{
    auto& found_target = get_target(target, location);
    const bool USE_BUILD_DIR {true};

    compile_target(found_target, USE_BUILD_DIR, location);
    link_target(found_target, USE_BUILD_DIR, location);
}

void restart_itself(const std::string& binary_name)
{
    std::println("Restarting with new binary: {}", binary_name);
    execl(binary_name.c_str(), binary_name.c_str(), (char*)nullptr);
    exit(0);
}

void self_rebuild(const std::filesystem::path& nobs_build_script_source_file, const Meta& meta_info)
{
    Target nobs_target;
    nobs_target.name = nobs_build_script_source_file.filename().stem();
    nobs_target.compile_flags.push_back("--std=c++23");
    nobs_target.compile_flags.push_back("-I ../..");
    nobs_target.needs_linking = true;
    nobs_target.sources.push_back(nobs_build_script_source_file.filename());

    const bool DONT_USE_BUILD_DIR {false};
    compile_target(nobs_target, DONT_USE_BUILD_DIR);
    link_target(nobs_target, DONT_USE_BUILD_DIR);

    make_meta_file(nobs_build_script_source_file, build_directory.parent_path(), meta_info);
    std::println("Build binary rebuilt. Restarting build application!");
    restart_itself(nobs_target.name);
}

void enable_self_rebuild(const std::source_location& location = std::source_location::current())
{
    std::filesystem::path nobs_build_script_source{location.file_name()};
    std::println("Nobs self rebuild active. File {} will be checked for changes every time build process is run", std::filesystem::canonical(nobs_build_script_source).string());

    auto new_nobs_source_meta = prepare_new_meta_for_file(nobs_build_script_source, "--std=c++23");
    
    auto old_nobs_source_meta_filename = get_file_metafile_name(nobs_build_script_source, ".");

    if (std::filesystem::exists(old_nobs_source_meta_filename))
    {
        auto old_meta = read_meta_info_from_file(old_nobs_source_meta_filename);

        if (old_meta == new_nobs_source_meta)
        {
            std::println("Nobs itself does not neeed to be rebuilt.");
            return;
        }
        else
        {
            std::println("There are changes in build script so build application will be rebuild.");
            self_rebuild(nobs_build_script_source, new_nobs_source_meta);
        }
    }
    else
    {
        std::println("Nobs meta data not present. Assuming first run.");
        make_meta_file(nobs_build_script_source, build_directory.parent_path(), new_nobs_source_meta);
    }

}

void DEBUG_list()
{
    std::println("Project_dir:\t{}", std::filesystem::canonical(project_directory).string());
    
    create_directory_if_missing(build_directory);
    std::println("Build_dir:\t{}", std::filesystem::canonical(build_directory).string());

    for (const auto& t : targets)
    {
        std::println("Target:\t\t{}", t.name);
        std::println("\nSources:");

        for (const auto& s : t.sources)
        {
            std::println("\t\t{}", std::filesystem::canonical(s).string());
        }
    }

}

}  // namespace nobs
