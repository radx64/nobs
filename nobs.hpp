#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <print>
#include <ranges>
#include <source_location>
#include <sstream>
#include <string_view>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace nobs {struct Target;}

// TODO: 
// For implementation of dependencies between libraries
// 1. Compile commands should be global and target should be able to add other target if needed
//      like executable need lib so libs should be compiled before
// Jobs should have a reference to a target and target description should be stateless
// Target state?
// 2. Change in linked lib should make target needs relinking


namespace nobs::internal
{
    static std::string compiler = "g++";
    static std::string linker = "g++";

    // TODO: make some parts of code be hidden by some internal namespace
    // to avoid polluting nobs namespace

    constexpr auto RESET_FONT = "\033[0m";
    constexpr auto RED_FONT   = "\033[31;1m";
    constexpr auto GREEN_FONT = "\033[32;1m";
    constexpr auto GREEN_FONT_FAINT = "\033[32;2m";
    constexpr auto YELLOW_FONT = "\033[33;1m";
    constexpr auto BLUE_FONT  = "\033[34;1m";

    constexpr auto metafile_extension = ".meta";
    constexpr auto object_file_extension = ".o";
    constexpr auto default_build_directory = "./build_dir";
    constexpr auto default_cpp_standard = "--std=c++23";
    constexpr auto current_directory = ".";
    constexpr auto compile_flag = "-c";
    constexpr auto compile_output_flag = "-o";
    constexpr auto linker_output_flag = "-o";

class Job
{
public:
    enum class Status { Pending, Running, Completed, Failed };
    Status status {Status::Pending};
    std::optional<uint32_t> exit_code { std::nullopt };
};

struct CompileParameters
{
    std::filesystem::path source_file;
    std::filesystem::path object_file;
    std::string compile_flags;
    uint64_t source_timestamp;
};

bool operator==(const CompileParameters& lhs, const CompileParameters& rhs)
{
    return lhs.source_file == rhs.source_file and
        lhs.object_file == rhs.object_file and
        lhs.compile_flags == rhs.compile_flags and
        lhs.source_timestamp == rhs.source_timestamp;
}

struct CompileJob : public Job
{
    CompileJob(const CompileParameters& compile_params) : params(compile_params) {}
    
    CompileParameters params;
};

struct LinkParameters
{
    std::vector<std::filesystem::path> object_files;
    std::filesystem::path target_file;
    std::string link_flags;
};

struct LinkJob : public Job
{
    LinkJob(const LinkParameters& link_params) : params(link_params) {}
    LinkParameters params;
};

} // namespace nobs::internal

namespace nobs
{
struct Target
{
    std::string name;
    enum class Type { Executable, StaticLib} type;
    std::vector<std::filesystem::path> sources;
    std::vector<std::string> compile_flags;

    Target() = default;
    Target(const std::string_view& target_name, const Target::Type target_type) : name(target_name), type(target_type) 
    {}

    Target(Target&& rhs) = default;
    Target& operator=(Target&& rhs) = default;

protected:
    Target(const Target& rhs) = default;
    Target& operator=(const Target& rhs) = default;
};
}  // namespace nobs

namespace nobs::internal
{
static std::vector<Target> targets {};
    
struct TargetBuildState
{
    const Target& target;
    std::vector<CompileJob> compile_jobs{};
    LinkJob link_job{LinkParameters{}};

    bool needs_linking {false};

    bool has_compilation_finished() const
    {
        return std::all_of(compile_jobs.begin(), compile_jobs.end(), [](const Job& job) {
            return job.status == Job::Status::Completed;
        });
    }

    bool has_linking_finished() const
    {
        return link_job.status == Job::Status::Completed;
    }

    std::vector<std::reference_wrapper<TargetBuildState>> depends_on_targets{};
};

static std::vector<TargetBuildState> target_build_states{};

static std::filesystem::path build_directory {default_build_directory};  // build in "build_dir" by default
static std::filesystem::path project_directory {std::filesystem::current_path()};
static bool clean_mode {false};
static size_t parallel_jobs = std::thread::hardware_concurrency();

void set_parallel_jobs(size_t num_jobs)
{
    parallel_jobs = num_jobs > 0 ? num_jobs : 1;
}

void trace_error(const std::string_view& error_string, const std::source_location location = std::source_location::current())
{
    std::println("{}Error at {}:{}: {}{}", RED_FONT, location.file_name(), location.line(), error_string, RESET_FONT);
}

inline std::vector<char*> build_argv(const std::vector<std::string>& command)
{
    std::vector<char*> argv;

    for (const auto& arg : command)
    {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);

    std::println("Executing command:{}", argv[1]);

    return argv;
}

inline void print_job_status(int percent, size_t ordinal, size_t total, std::string_view color, std::string_view type, const std::vector<std::string>& command)
{
    std::string command_str;
    for (size_t i = 0; i < command.size(); ++i) {
        command_str += command[i];
        if (i < command.size() - 1) command_str += " ";
    }

    std::println("[{:3}%] {}/{} {}{} {}{}", percent, ordinal, total, color, type, command_str, RESET_FONT);
}

inline int compute_percent(size_t completed, size_t pending, size_t jobs_count)
{
    return static_cast<int>((completed + pending + 1) * 100 / jobs_count);
}

void create_directory_if_missing(const std::filesystem::path& directory)
{
    try
    { 
        std::filesystem::create_directories(directory);
    }
    catch (std::filesystem::filesystem_error& error)
    {
        trace_error(std::format("ERROR: got {} - code {}", error.what(), error.code().message()));
        throw;
    }
}

CompileParameters read_compile_parameters_from_file(const std::filesystem::path& job_metafile)
{
    std::string job_metafile_name = job_metafile.string();
    std::ifstream file{job_metafile_name};

    if (not file)
    {
        trace_error(std::format("Error opening file {}", job_metafile_name), std::source_location::current());
        exit(1);
    }

    CompileParameters job{};

    std::string line{};

    if (not std::getline(file, line)) {
        trace_error(std::format("Could not read source file from metafile {}", job_metafile_name));
        exit(1);
    }
    job.source_file = line.c_str();

    if (not std::getline(file, line)) {
        trace_error(std::format("Could not read object source file from metafile {}", job_metafile_name));
        exit(1);
    }
    job.object_file = line.c_str();

    if (not std::getline(file, line)) {
        trace_error(std::format("Could not read compiler flags from metafile {}", job_metafile_name));
        exit(1);
    }
    job.compile_flags = line.c_str();

    if (not std::getline(file, line)) {
        trace_error(std::format("Could not read timestamp from metafile {}", job_metafile_name));
        exit(1);
    }
    job.source_timestamp = std::stoull(line.c_str());
    return job;
}

auto get_file_metafile_name(const std::filesystem::path& source_file, const std::filesystem::path& build_source_path)
{
    auto meta_file = std::filesystem::canonical(build_source_path);
    meta_file /= source_file.filename();
    meta_file += metafile_extension;
    return meta_file;
}

void save_meta_file(const CompileParameters& compile_job)
{
    const auto meta_file = compile_job.object_file.string() + metafile_extension;
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

TargetBuildState& get_target_build_state(const Target& target)
{
    for (auto& tbs : target_build_states)
    {
        if (tbs.target.name == target.name)
        {
            return tbs;
        }
    }
    target_build_states.push_back(TargetBuildState{target});
    return target_build_states.back();
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
    object_file = std::filesystem::path{object_file.string() + object_file_extension};

    auto metafile_name = get_file_metafile_name(object_file, build_source_path);
    
    CompileParameters new_compile_parameters{
        .source_file = relative_source_path,
        .object_file = object_file,
        .compile_flags = flags,
        .source_timestamp = get_file_timestamp(source),
    };
    
    if (std::filesystem::exists(metafile_name))
    {
        auto old_compile_parameters = read_compile_parameters_from_file(metafile_name);
        if (old_compile_parameters == new_compile_parameters)
        {
            // TODO add verbosity level to print that file is up to date
            return;
        }
    }

    auto& target_build_state = get_target_build_state(target);
    target_build_state.compile_jobs.push_back(CompileJob{new_compile_parameters});
    target_build_state.needs_linking = true; // TODO: when this is set, all targets depending on this one should also be marked for relinking
}

void prepare_target_compilation(Target& target, const bool use_build_dir = true)
{
    create_directory_if_missing(build_directory);
    
    std::string flags{};
    for (size_t i = 0; i < target.compile_flags.size(); ++i)
    {
        flags.append(target.compile_flags[i]);
        if (i < target.compile_flags.size() - 1)
        {
            flags.append(" ");
        }
    }

    for (const auto& source : target.sources)
    {    
        prepare_file_compilation(target, flags, use_build_dir, source);
    }
}

void prepare_target_linking(Target& target, const bool use_build_dir = true)
{
    auto& target_build_state = get_target_build_state(target);

    if (not target_build_state.needs_linking)
    {
        return;
    }

    auto canonical_build_dir = std::filesystem::canonical(build_directory);
    if (not use_build_dir) canonical_build_dir = std::filesystem::canonical(current_directory);

    auto link_params = LinkParameters{};

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

        auto build_source_object_file = (canonical_build_dir / relative_source_path).string() + object_file_extension;
        link_params.object_files.push_back(build_source_object_file);
    }

    link_params.target_file = (canonical_build_dir / target.name);
    link_params.link_flags = ""; // TODO add link flags support to Target
    target_build_state.link_job = LinkJob{link_params};
}

struct PendingProcess {
    size_t job_index;
    pid_t pid;
    bool is_compile_job;
};

std::vector<std::string> build_command_for_compile_job(const CompileJob& compile_job)
{
    std::vector<std::string> command_args{};
    command_args.push_back(internal::compiler);
    command_args.push_back(compile_job.params.compile_flags);

    std::istringstream iss(compile_job.params.compile_flags);
    std::string flag;
    while (iss >> flag)
    {
        command_args.push_back(flag);
    }

    command_args.push_back(internal::compile_flag);
    command_args.push_back(internal::compile_output_flag);
    command_args.push_back(compile_job.params.object_file.string());
    command_args.push_back(compile_job.params.source_file.string());

    return command_args;
}

std::vector<std::string> build_command_for_link_job(const LinkJob& link_job)
{
    std::vector<std::string> command_args{};
    command_args.push_back(internal::linker);
    command_args.push_back(internal::linker_output_flag);
    command_args.push_back(link_job.params.target_file.string());

    for (const auto& obj : link_job.params.object_files)
    {
        command_args.push_back(obj.string());
    }

    return command_args;
}

void run_build(const Target& target)
{
    // TODO: redesign it, so complie and link jobs are added to global queue
    // which is later consumed by worker processes and build is done in parallel for all targets
    // but this will be done later

    auto& target_build_state = get_target_build_state(target);

    const auto jobs_count = target_build_state.compile_jobs.size();
    if (jobs_count == 0)
    {
        std::println("{}Nothing to build for target {}{}{}.{}", internal::GREEN_FONT, internal::RED_FONT, target.name, internal::GREEN_FONT, internal::RESET_FONT);
        return;
    }

    std::println("{}Running build of {}{}{} with {} jobs (max {} parallel)...{}", internal::GREEN_FONT, internal::RED_FONT, target.name, internal::GREEN_FONT, jobs_count, internal::parallel_jobs, internal::RESET_FONT);

    std::vector<PendingProcess> pending_processes;
    size_t completed_jobs = 0;
    bool link_job_added = false;

    while (completed_jobs < jobs_count)
    {
        // Check for completed processes
        for (auto it = pending_processes.begin(); it != pending_processes.end(); )
        {
            int status;
            pid_t result = waitpid(it->pid, &status, WNOHANG);
            
            if (result == it->pid)
            {
                int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
                
                if (exit_code != 0)
                {
                    std::println("{}Error: Command failed with code {}. Stopping build.{}", internal::RED_FONT, exit_code, internal::RESET_FONT);
                    exit(exit_code);
                }
                
                completed_jobs++;
                
                if (it->is_compile_job)
                {
                    target_build_state.compile_jobs[it->job_index].status = Job::Status::Completed;
                    save_meta_file(target_build_state.compile_jobs[it->job_index].params);
                }
                else
                {
                    target_build_state.link_job.status = Job::Status::Completed;
                    std::println("{}Linking completed successfully.{}", internal::GREEN_FONT, internal::RESET_FONT);
                }
                
                it = pending_processes.erase(it);
            }
            else
            {
                ++it;
            }
        }
        
        // Spawn new compile jobs if we have capacity
        while (pending_processes.size() < internal::parallel_jobs && completed_jobs + pending_processes.size() < target_build_state.compile_jobs.size())
        {
            size_t index = completed_jobs + pending_processes.size();
            auto& compile_job = target_build_state.compile_jobs[index];
            
            if (compile_job.status == Job::Status::Pending)
            {
                compile_job.status = Job::Status::Running;
                
                auto percent = compute_percent(completed_jobs, pending_processes.size(), jobs_count + 1);
                auto command_args = build_command_for_compile_job(compile_job);
                print_job_status(percent, completed_jobs + pending_processes.size(), jobs_count, internal::GREEN_FONT_FAINT, "Compiling", command_args);
                
                pid_t pid = fork();
                if (pid == 0)
                {
                    auto argv = build_argv(command_args);
                    execvp(argv[0], argv.data());
                    exit(-1);
                }
                
                pending_processes.push_back({index, pid, true});
            }
        }
        
        // Spawn link job after all compile jobs are done
        if (!link_job_added && target_build_state.has_compilation_finished() && target_build_state.needs_linking)
        {
            target_build_state.link_job.status = Job::Status::Running;
            link_job_added = true;
            
            auto percent = compute_percent(completed_jobs, pending_processes.size(), jobs_count + 1);
            std::vector<std::string> command_args = build_command_for_link_job(target_build_state.link_job);
            print_job_status(percent, completed_jobs + pending_processes.size(), jobs_count, internal::GREEN_FONT, "Linking", command_args);
            
            pid_t pid = fork();
            if (pid == 0)
            {
                auto argv = build_argv(command_args);
                execvp(argv[0], argv.data());
                exit(-1);
            }
            else
            {
                int status;
                waitpid(pid, &status, 0);
            }
            pending_processes.push_back({0, pid, false});
        }
        
        if (!pending_processes.empty())
        {
            usleep(10000);
        }
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
            object_file = std::filesystem::path{(canonical_build_dir / source).string() + object_file_extension};
        }
        else
        {
            object_file = std::filesystem::path{source.string() + object_file_extension};
        }

        std::filesystem::remove(object_file);
    }
}

}  // namespace nobs::internal


namespace nobs
{

void add_target_include_directories(Target& target, const std::vector<std::string_view>& include_dirs)
{
    for (const auto& dir : include_dirs)
    {
        target.compile_flags.push_back(std::format("-I{}", dir));
    }    
}

void set_compiler(const std::string_view& compiler_name)
{
    internal::compiler = std::string(compiler_name);
}

void set_linker(const std::string_view& linker_name)
{
    internal::linker = std::string(linker_name);
}

void enable_command_line_params(const int argc, const char* argv[])
{
    if (argc == 1) return;

    for (int i = 1; i < argc; ++i)
    {
        auto param = std::string(argv[i]);

        if (param == "--help" || param == "-h")
        {
            std::println("usage: {}", argv[0]);
            std::println("  -c, --clean\t- cleans build artifacts");
            std::println("  -m, --jobs N\t- use N parallel jobs (default: {})", internal::parallel_jobs);
            std::println("  -h, --help\t- shows this help");
            exit(0);
        }
        else if (param == "--clean" || param == "-c")
        {
            internal::clean_mode = true;
        }
        else if (param == "--jobs" || param == "-m")
        {
            if (i + 1 < argc)
            {
                try
                {
                    size_t num_jobs = std::stoull(argv[i + 1]);
                    internal::set_parallel_jobs(num_jobs);
                    ++i;  // Skip the next argument since we consumed it
                }
                catch (const std::exception& e)
                {
                    internal::trace_error(std::format("Invalid number of jobs: {}", argv[i + 1]));
                    exit(1);
                }
            }
            else
            {
                internal::trace_error("--jobs/-m requires an argument");
                exit(1);
            }
        }
    }
}

Target& add_executable(const std::string_view& name)
{
    return internal::targets.emplace_back(name, Target::Type::Executable);
}

void set_build_directory(const std::string_view& build_dir)
{
    internal::build_directory = std::string(build_dir);
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
            internal::trace_error(std::format("Source file {} does not exist!", source), location);
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
    const std::vector<std::string_view>& flags)
{
    for (const auto& flag : flags)
    {
        target.compile_flags.push_back(std::string(flag));
    }    
}

void add_target_compile_flag(Target& target,
    const std::string_view& flag)
{
    add_target_compile_flags(target, {flag});
}

void build_target(Target& target)
{
    if (internal::clean_mode)
    {
        std::filesystem::remove_all(internal::build_directory);
    }
    else 
    {
        const bool USE_BUILD_DIR {true};

        internal::prepare_target_compilation(target, USE_BUILD_DIR);
        internal::prepare_target_linking(target, USE_BUILD_DIR);
        internal::run_build(target);
    }
}

void enable_self_rebuild(const std::source_location& location = std::source_location::current())
{
    std::filesystem::path nobs_build_script_source {location.file_name()};
    std::println("{}Nobs self rebuild active. File {} will be checked for changes every time build process is run {}", 
        internal::YELLOW_FONT, std::filesystem::canonical(nobs_build_script_source).string(), internal::RESET_FONT);

    auto& nobs_executable = add_executable(nobs_build_script_source.filename().stem().string());
    
    add_target_source(nobs_executable, nobs_build_script_source.string());
    add_target_compile_flag(nobs_executable, internal::default_cpp_standard);
    const bool DONT_USE_BUILD_DIR {false};
    internal::prepare_target_compilation(nobs_executable, DONT_USE_BUILD_DIR);
    internal::prepare_target_linking(nobs_executable, DONT_USE_BUILD_DIR);

    auto& nobs_executable_build_state = internal::get_target_build_state(nobs_executable);

    if (nobs_executable_build_state.needs_linking == false)
    {
        std::println("{}Nobs build script has not changed. No need to rebuild.{}", internal::GREEN_FONT, internal::RESET_FONT);
        return;
    }
    internal::run_build(nobs_executable);
    internal::clean_target_build_artifacts(nobs_executable, DONT_USE_BUILD_DIR);
    internal::restart_itself(nobs_executable.name);
}

void set_project_directory(const std::string_view& project_dir)
{
    internal::project_directory = std::filesystem::path(project_dir);
}

std::string current_project_directory()
{
    return internal::project_directory.string();
}

Target& add_library(const std::string_view& name)
{
    return internal::targets.emplace_back(name, Target::Type::StaticLib);
}

void target_link_libraries(Target& target, const std::vector<std::reference_wrapper<Target>> libraries)
{

}

} // namespace nobs
