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
#include <variant>
#include <unistd.h>
#include <utility>

namespace nobs
{
    auto constexpr compiler = "g++";
    auto constexpr linker = "g++";

    constexpr auto RESET_FONT = "\033[0m";
    constexpr auto RED_FONT   = "\033[31;1m";
    constexpr auto GREEN_FONT = "\033[32;1m";
    constexpr auto GREEN_FONT_FAINT = "\033[32;2m";
    constexpr auto YELLOW_FONT = "\033[33;1m";
    constexpr auto BLUE_FONT  = "\033[34;1m";

struct CompileJob
{
    std::filesystem::path source_file;
    std::filesystem::path object_file;
    std::string compile_flags;
    uint64_t source_timestamp;
};

bool operator==(const CompileJob& lhs, const CompileJob& rhs)
{
    return lhs.source_file == rhs.source_file and
        lhs.object_file == rhs.object_file and
        lhs.compile_flags == rhs.compile_flags and
        lhs.source_timestamp == rhs.source_timestamp;
}

struct LinkJob
{
    std::vector<std::filesystem::path> object_files;
    std::filesystem::path target_file;
    std::string link_flags;
};

struct Job
{
    std::variant<CompileJob, LinkJob> specific_job;
    // TODO add dependencies between jobs (eg to link target after all files are compiled)
};

struct Target
{
    std::string name;
    std::vector<std::filesystem::path> sources;
    std::vector<std::string> compile_flags;
    std::vector<Job> build_jobs{};
    bool needs_linking {false};

    Target() = default;
    Target(const std::string_view& target_name) : name(target_name) {}

    Target(Target&& rhs) = default;
    Target& operator=(Target&& rhs) = default;

protected:
    Target(const Target& rhs) = default;
    Target& operator=(const Target& rhs) = default;
};

static std::vector<Target> targets {};
static std::filesystem::path build_directory {"./build_dir"};  // build in "build_dir" by default
static std::filesystem::path project_directory {std::filesystem::current_path()};
static bool clean_mode{false};

void set_project_directory(const std::string_view& project_dir)
{
    project_directory = std::filesystem::path(project_dir);
}

std::string current_project_directory()
{
    return project_directory.string();
}

void set_build_directory(const std::string_view& build_dir)
{
    build_directory = std::string(build_dir);
}

void trace_error(const std::string_view& error_string, const std::source_location location = std::source_location::current())
{
    std::println("{}Error at {}:{}: {}{}", RED_FONT, location.file_name(), location.line(), error_string, RESET_FONT);
}

Target& add_executable(const std::string_view& name)
{
    return targets.emplace_back(name);
}

void create_directory_if_missing(const std::filesystem::path directory)
{
    try
    { 
        auto result = std::filesystem::create_directories(directory);
    }
    catch (std::filesystem::filesystem_error& error)
    {
        trace_error(std::format("ERROR: got {} - code {}", error.what(), error.code().message()));
        throw;
    }
}

void add_target_sources(Target& target, 
    const std::vector<std::string_view>& sources, 
    const std::source_location location = std::source_location::current())
{
    for (const auto& source : sources)
    {
        if (std::filesystem::exists(source))
        {
            target.sources.push_back(std::filesystem::path(source));
        }
        else
        {
            trace_error(std::format("Source file {} does not exist!", source), location);
            exit(1);
        }
    }
}

void add_target_source(Target& target,
    const std::string_view& source, 
    const std::source_location location = std::source_location::current())
{
    add_target_sources(target, {source}, location);
}

void add_target_compile_flags(Target& target,
    const std::vector<std::string_view>& flags,
    const std::source_location location = std::source_location::current())
{
    for (const auto& flag : flags)
    {
        target.compile_flags.push_back(std::string(flag));
    }    
}

void add_target_compile_flag(Target& target,
    const std::string_view& flag,
    const std::source_location location = std::source_location::current())
{
    add_target_compile_flags(target, {flag}, location);
    
}

CompileJob read_compile_job_from_file(const std::filesystem::path& job_metafile)
{
    std::string job_metafile_name = job_metafile.string();
    std::ifstream file{job_metafile_name};

    if (not file)
    {
        trace_error(std::format("Error opening file {}", job_metafile_name), std::source_location::current());
        exit(1);
    }

    CompileJob job{};

    std::string line{};

    if (not std::getline(file, line)) trace_error(std::format("Could not read source file from metafile {}", job_metafile_name));
    job.source_file = line.c_str();

    if (not std::getline(file, line)) trace_error(std::format("Could not read object source file from metafile {}", job_metafile_name));
    job.object_file = line.c_str();

    if (not std::getline(file, line)) trace_error(std::format("Could not read compiler flags from metafile {}", job_metafile_name));
    job.compile_flags = line.c_str();

    if (not std::getline(file, line)) trace_error(std::format("Could not read timestamp from metafile {}", job_metafile_name));
    job.source_timestamp = std::stoull(line.c_str());
    return job;
}

auto get_file_metafile_name(const std::filesystem::path& source_file, const std::filesystem::path& build_source_path)
{
    auto meta_file = std::filesystem::canonical(build_source_path);
    meta_file /= source_file.filename();
    meta_file += ".meta";
    return meta_file;
}

void write_compile_job_to_file(const CompileJob& compile_job)
{
    auto meta_file = compile_job.object_file.string() + ".meta";

    if (std::ofstream file{meta_file.c_str()}; file) {
        std::println(file, "{}", compile_job.source_file.string());
        std::println(file, "{}", compile_job.object_file.string());
        std::println(file, "{}", compile_job.compile_flags);
        std::println(file, "{}", compile_job.source_timestamp);
    } else {
        trace_error("Error opening file!");
        exit(-1);
    } 
}

uint64_t get_file_timestamp(const std::filesystem::path& filename)
{
    if (std::filesystem::exists(filename))
    {
        return static_cast<uint64_t>(std::filesystem::last_write_time(filename).time_since_epoch().count());
    }
    else
    {
        return 0;
    }
}

void prepare_file_compilation(Target& target, const std::string& flags, const bool use_build_dir, const std::filesystem::path& source)
{
    create_directory_if_missing(build_directory);
    auto canonical_build_dir = std::filesystem::canonical(build_directory);

    std::filesystem::path relative_source_path{};
    if (source.is_absolute())
    {
        relative_source_path = std::filesystem::relative(source, project_directory);
    }
    else
    {
        relative_source_path = source;
    }

    auto build_source_file = canonical_build_dir / relative_source_path;
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

    auto metafile_name = get_file_metafile_name(object_file, build_source_path);
    
    CompileJob new_compile_job{
        .source_file = relative_source_path,
        .object_file = object_file,
        .compile_flags = flags,
        .source_timestamp = get_file_timestamp(source),
    };
    
    if (std::filesystem::exists(metafile_name))
    {
        auto old_compile_job = read_compile_job_from_file(metafile_name);
        if (old_compile_job == new_compile_job)
        {
            // TODO add verbosity level to print that file is up to date
            return;
        }
    }

    target.needs_linking = true;
    target.build_jobs.push_back(Job{new_compile_job});
}

void prepare_target_compilation(Target& target, const bool use_build_dir = true, const std::source_location location = std::source_location::current())
{
    create_directory_if_missing(build_directory);
    
    std::string flags{};
    for (const auto & flag : target.compile_flags)
    {  
        flags.append(std::format("{} ", flag));
    }

    for (const auto& source : target.sources)
    {    
        prepare_file_compilation(target, flags, use_build_dir, source);
    }
}

void prepare_target_linking(Target& target, const bool use_build_dir = true, const std::source_location location = std::source_location::current())
{
    if (not target.needs_linking)
    {
        return;
    }

    auto canonical_build_dir = std::filesystem::canonical(build_directory);
    if (not use_build_dir) canonical_build_dir = std::filesystem::canonical(".");

    auto link_job = LinkJob{};

    for (const auto& source : target.sources)
    {
        // Determine object file path
        std::filesystem::path relative_source_path{};
        if (source.is_absolute())
        {
            relative_source_path = std::filesystem::relative(source, project_directory);
        }
        else
        {
            relative_source_path = source;
        }

        auto build_source_object_file = (canonical_build_dir / relative_source_path) += ".o";
        link_job.object_files.push_back(build_source_object_file);
    }

    link_job.target_file = (canonical_build_dir / target.name);
    link_job.link_flags = ""; // TODO add link flags support to Target
    target.build_jobs.push_back(Job{.specific_job = link_job});
}

void run_build(const Target& target)
{
    auto jobs_count = target.build_jobs.size();
    if (jobs_count == 0)
    {
        std::println("{}Nothing to build for target {}{}{}.{}", GREEN_FONT, RED_FONT, target.name, GREEN_FONT, RESET_FONT);
        return;
    }
    std::println("{}Running build of {}{}{} with {} jobs...{}", GREEN_FONT, RED_FONT, target.name, GREEN_FONT, jobs_count, RESET_FONT);
    for (const auto& [index, job] : std::views::enumerate(target.build_jobs))
    {  
        auto is_compile_job = std::holds_alternative<CompileJob>(job.specific_job);

        auto percent = static_cast<int>((index + 1) * 100 / jobs_count);
        std::string type{};
        auto color = RED_FONT;
        std::string command{};

        if (is_compile_job)
        {
            type = "Compliling";
            color = GREEN_FONT_FAINT;
            auto specific_job = std::get<CompileJob>(job.specific_job);
            command = std::format("{} {} -c -o {} {}", compiler, specific_job.compile_flags, specific_job.object_file.string(), specific_job.source_file.string());
        }
        else
        {
            type = "Linking";
            color = GREEN_FONT;
            auto specific_job = std::get<LinkJob>(job.specific_job);
            auto link_objects = std::string{};

            for (const auto& object : specific_job.object_files)
            {
                link_objects.append(std::string(" ") + object.string());
            }

            command = std::format("{} -o {} {} ", compiler, specific_job.target_file.string(), link_objects);
        }

        std::println("[{:3}%] {}/{} {}{} {}{}", percent, index+1, jobs_count, color, type, command, RESET_FONT);
        auto result = std::system(command.c_str());  // todo: replace system with something more sophisticated (to let parallel execution)
        if (result != 0)
        {
            std::println("{}Error: Command failed with code {}. Stopping build.{}", RED_FONT, result, RESET_FONT);
            exit(result);
        }
        
        if (is_compile_job)
        {
            auto specific_job = std::get<CompileJob>(job.specific_job);
            write_compile_job_to_file(specific_job);
        }
    }
}

void build_target(Target& target, const std::source_location location = std::source_location::current())
{
    if (clean_mode)
    {
        std::filesystem::remove_all(build_directory);
    }
    else 
    {
        const bool USE_BUILD_DIR {true};

        prepare_target_compilation(target, USE_BUILD_DIR, location);
        prepare_target_linking(target, USE_BUILD_DIR, location);
        run_build(target);
    }
}

void restart_itself(const std::string& binary_name)
{
    std::println("{}Restarting with new binary: {}{}{}", YELLOW_FONT, RED_FONT, binary_name, RESET_FONT);
    execl(binary_name.c_str(), binary_name.c_str(), (char*)nullptr);
    exit(0);
}

void clean_target_build_artifacts(const Target& target, const bool use_build_dir)
{
    for (const auto& source : target.sources)
    {
        std::filesystem::path object_file{};
        if (use_build_dir)
        {
            auto canonical_build_dir = std::filesystem::canonical(build_directory);
            object_file = (canonical_build_dir / source) += ".o";
        }
        else
        {
            (object_file = source) += ".o";
        }

        std::filesystem::remove(object_file);
    }
}


void enable_self_rebuild(const std::source_location& location = std::source_location::current())
{
    std::filesystem::path nobs_build_script_source {location.file_name()};
    std::println("{}Nobs self rebuild active. File {} will be checked for changes every time build process is run {}", 
        YELLOW_FONT, std::filesystem::canonical(nobs_build_script_source).string(), RESET_FONT);

    auto& nobs_executable = add_executable(nobs_build_script_source.filename().stem().string());
    
    add_target_source(nobs_executable, nobs_build_script_source.string());
    add_target_compile_flag(nobs_executable, "--std=c++23");
    const bool DONT_USE_BUILD_DIR {false};
    prepare_target_compilation(nobs_executable, DONT_USE_BUILD_DIR);
    prepare_target_linking(nobs_executable, DONT_USE_BUILD_DIR);

    if (nobs_executable.needs_linking == false)
    {
        std::println("{}Nobs build script has not changed. No need to rebuild.{}", GREEN_FONT, RESET_FONT);
        return;
    }
    run_build(nobs_executable);
    clean_target_build_artifacts(nobs_executable, DONT_USE_BUILD_DIR);
    restart_itself(nobs_executable.name);
}

void enable_command_line_params(const int argc, const char* argv[])
{
    if (argc == 1) return;

    auto first_param = std::string(argv[1]);

    if (first_param == "--help" || first_param == "-h" )
    {
        std::println("usage: {}", argv[0]);
        std::println("  -c, --clean\t- cleans build artifacts");
        std::println("  -h, --help\t- shows this help");
        exit(0);
    }

    if (first_param == "--clean" || first_param == "-c")
    {
        clean_mode = true;
    }
}

}  // namespace nobs
