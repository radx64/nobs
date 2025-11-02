#include <print>
#include <ranges>
#include <string_view>
#include <string>
#include <vector>
#include <source_location>
#include <format>
#include <filesystem>
#include <fstream>

namespace nobs
{

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
};

bool operator==(const Meta& lhs, const Meta& rhs)
{
    return (lhs.timestamp == rhs.timestamp) 
        and (lhs.compile_flags == rhs.compile_flags);
}

static std::vector<Target> targets {};
static std::filesystem::path build_directory {"./build_dir"};  // build in "build" by default
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

void compile_file(Target& target, const std::string& compiler, const std::string& flags, const std::filesystem::path& source)
{
    auto build_source_file = build_directory / source;
    auto build_source_path = build_source_file.parent_path();
    create_directory_if_missing(build_source_path);

    auto object_file = std::filesystem::canonical(build_source_path);
    object_file /= source.filename();
    object_file += ".o";

    auto meta_file = get_file_metafile_name(source, build_source_path);

    auto new_file_meta_info = prepare_new_meta_for_file(source, flags);

    if (std::filesystem::exists(meta_file))
    {
        auto already_built_meta_info = read_meta_info_from_file(meta_file);

        if (already_built_meta_info == new_file_meta_info)
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

void compile_target(Target& target, const std::source_location location = std::source_location::current())
{
    create_directory_if_missing(build_directory);

    auto compiler = "g++"; 
    
    std::string flags{};
    for (const auto & flag : target.compile_flags)
    {  
        flags.append(std::format("{} ", flag));
    }

    for (const auto& source : target.sources)
    {    
        compile_file(target, compiler, flags, source);
    }
}

void link_target(const Target& target, const std::source_location location = std::source_location::current())
{
    if (not target.needs_linking)
    {
        std::println("No need to relink {}", target.name);
        return;
    }

    auto linker = "g++";

    auto canonical_build_dir = std::filesystem::canonical(build_directory);

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
    auto& found = get_target(target, location);

    compile_target(found, location);
    link_target(found, location);
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
