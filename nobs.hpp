#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
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

namespace nobs
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
    std::vector<size_t> depends_on;  // indices of jobs this job depends on
    enum class Status { Pending, Running, Completed, Failed } status = Status::Pending;
    int exit_code = 0;
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
static std::filesystem::path build_directory {default_build_directory};  // build in "build_dir" by default
static std::filesystem::path project_directory {std::filesystem::current_path()};
static bool clean_mode{false};
static size_t parallel_jobs = std::thread::hardware_concurrency();

struct PendingJob {
    size_t job_index;
    pid_t pid;
    std::string command_display;
    bool is_compile_job;
};

void set_compiler(const std::string_view& compiler_name)
{
    compiler = std::string(compiler_name);
}

void set_linker(const std::string_view& linker_name)
{
    linker = std::string(linker_name);
}

void set_parallel_jobs(size_t num_jobs)
{
    parallel_jobs = num_jobs > 0 ? num_jobs : 1;
}

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

int execute_command(const std::vector<std::string>& args)
{
    pid_t pid = fork();
    if (pid == -1)
    {
        trace_error("Failed to fork process");
        exit(-1);
    }

    if (pid == 0)
    {
        // Child process
        auto argv = build_argv(args);
        execvp(argv[0], argv.data());
        // If execvp returns, an error occurred
        trace_error("Failed to execute command");
        exit(-1);
    }
    else
    {
        // Parent process
        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status))
        {
            return WEXITSTATUS(status);
        }
        else
        {
            trace_error("Child process did not terminate normally");
            return -1;
        }
    }
}

Target& add_executable(const std::string_view& name)
{
    return targets.emplace_back(name);
}

bool are_dependencies_satisfied(const std::vector<Job>& jobs, size_t job_index)
{
    const auto& job = jobs[job_index];
    for (size_t dep_index : job.depends_on)
    {
        if (jobs[dep_index].status != Job::Status::Completed)
        {
            return false;
        }
    }
    return true;
}

inline std::vector<char*> build_argv(const std::vector<std::string>& args)
{
    std::vector<char*> argv;
    for (const auto& arg : args)
    {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);
    return argv;
}

inline void print_job_status(int percent, size_t ordinal, size_t total, std::string_view color, std::string_view type, const std::string& command_display)
{
    std::println("[{:3}%] {}/{} {}{} {}{}", percent, ordinal, total, color, type, command_display, RESET_FONT);
}

inline std::pair<std::vector<std::string>, bool> build_job_command_args(const Job& job)
{
    std::vector<std::string> args;
    if (std::holds_alternative<CompileJob>(job.specific_job))
    {
        auto specific_job = std::get<CompileJob>(job.specific_job);
        args.push_back(compiler);
        std::istringstream iss(specific_job.compile_flags);
        std::string flag;
        while (iss >> flag)
        {
            args.push_back(flag);
        }
        args.push_back(compile_flag);
        args.push_back(compile_output_flag);
        args.push_back(specific_job.object_file.string());
        args.push_back(specific_job.source_file.string());
        return {args, true};
    }
    else
    {
        auto specific_job = std::get<LinkJob>(job.specific_job);
        args.push_back(compiler);
        args.push_back(linker_output_flag);
        args.push_back(specific_job.target_file.string());
        for (const auto& object : specific_job.object_files)
        {
            args.push_back(object.string());
        }
        return {args, false};
    }
}

inline std::string join_command_display(const std::vector<std::string>& args)
{
    std::string out;
    for (const auto& a : args) { out += a; out += ' '; }
    return out;
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

void write_compile_job_to_file(const CompileJob& compile_job)
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

void prepare_target_compilation(Target& target, const bool use_build_dir = true)
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

void prepare_target_linking(Target& target, const bool use_build_dir = true)
{
    if (not target.needs_linking)
    {
        return;
    }

    auto canonical_build_dir = std::filesystem::canonical(build_directory);
    if (not use_build_dir) canonical_build_dir = std::filesystem::canonical(current_directory);

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

        auto build_source_object_file = (canonical_build_dir / relative_source_path).string() + object_file_extension;
        link_job.object_files.push_back(build_source_object_file);
    }

    link_job.target_file = (canonical_build_dir / target.name);
    link_job.link_flags = ""; // TODO add link flags support to Target
    
    // Link job depends on all compile jobs
    Job link_job_with_deps{.specific_job = link_job};
    for (size_t i = 0; i < target.build_jobs.size(); ++i)
    {
        link_job_with_deps.depends_on.push_back(i);
    }
    target.build_jobs.push_back(link_job_with_deps);
}

void run_build(Target& target)
{
    const auto jobs_count = target.build_jobs.size();
    if (jobs_count == 0)
    {
        std::println("{}Nothing to build for target {}{}{}.{}", GREEN_FONT, RED_FONT, target.name, GREEN_FONT, RESET_FONT);
        return;
    }
    std::println("{}Running build of {}{}{} with {} jobs (max {} parallel)...{}", GREEN_FONT, RED_FONT, target.name, GREEN_FONT, jobs_count, parallel_jobs, RESET_FONT);
    
    std::vector<PendingJob> pending_jobs;
    size_t completed_jobs = 0;
    
    while (completed_jobs < jobs_count)
    {
        for (auto it = pending_jobs.begin(); it != pending_jobs.end(); )
        {
            int status;
            pid_t result = waitpid(it->pid, &status, WNOHANG);
            
            if (result == it->pid)  // Child process completed
            {
                auto& job = target.build_jobs[it->job_index];
                int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
                job.exit_code = exit_code;
                
                if (exit_code != 0)
                {
                    job.status = Job::Status::Failed;
                    std::println("{}Error: Command failed with code {}. Stopping build.{}", RED_FONT, exit_code, RESET_FONT);
                    exit(exit_code);
                }
                
                job.status = Job::Status::Completed;
                completed_jobs++;
                
                if (it->is_compile_job)
                {
                    auto specific_job = std::get<CompileJob>(job.specific_job);
                    write_compile_job_to_file(specific_job);
                }
                
                it = pending_jobs.erase(it);
            }
            else
            {
                ++it;
            }
        }
        
        // Spawn new jobs if we have capacity and dependencies are satisfied
        while (pending_jobs.size() < parallel_jobs && completed_jobs + pending_jobs.size() < jobs_count)
        {
            bool found_ready_job = false;
            
            for (size_t index = 0; index < jobs_count; ++index)
            {
                auto& job = target.build_jobs[index];
                
                if (job.status != Job::Status::Pending)
                {
                    continue;
                }
                
                if (!are_dependencies_satisfied(target.build_jobs, index))
                {
                    continue;
                }

                found_ready_job = true;

                auto [command_args, is_compile_job] = build_job_command_args(job);
                auto percent = compute_percent(completed_jobs, pending_jobs.size(), jobs_count);
                auto color = is_compile_job ? GREEN_FONT_FAINT : GREEN_FONT;
                auto type = is_compile_job ? "Compiling" : "Linking";

                std::string command_display = join_command_display(command_args);
                print_job_status(percent, completed_jobs + pending_jobs.size() + 1, jobs_count, color, type, command_display);

                job.status = Job::Status::Running;

                // Fork and execute the command
                pid_t pid = fork();
                if (pid == -1)
                {
                    trace_error("Failed to fork process");
                    exit(-1);
                }

                if (pid == 0)
                {
                    // Child process
                    auto argv = build_argv(command_args);
                    execvp(argv[0], argv.data());
                    // If execvp returns, an error occurred
                    trace_error("Failed to execute command");
                    exit(-1);
                }
                else
                {
                    // Parent process - track the job
                    pending_jobs.push_back({index, pid, command_display, is_compile_job});
                    break;  // Go back to check for completions
                }
            }
            
            if (!found_ready_job)
            {
                break;  // No more ready jobs, wait for some to complete
            }
        }
        
        // If we have pending jobs, wait a bit before checking again
        if (!pending_jobs.empty())
        {
            usleep(10000);  // 10ms sleep to avoid busy-waiting
        }
    }
}

void build_target(Target& target)
{
    if (clean_mode)
    {
        std::filesystem::remove_all(build_directory);
    }
    else 
    {
        const bool USE_BUILD_DIR {true};

        prepare_target_compilation(target, USE_BUILD_DIR);
        prepare_target_linking(target, USE_BUILD_DIR);
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
            object_file = std::filesystem::path{(canonical_build_dir / source).string() + object_file_extension};
        }
        else
        {
            object_file = std::filesystem::path{source.string() + object_file_extension};
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
    add_target_compile_flag(nobs_executable, default_cpp_standard);
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

    for (int i = 1; i < argc; ++i)
    {
        auto param = std::string(argv[i]);

        if (param == "--help" || param == "-h")
        {
            std::println("usage: {}", argv[0]);
            std::println("  -c, --clean\t- cleans build artifacts");
            std::println("  -m, --jobs N\t- use N parallel jobs (default: {})", parallel_jobs);
            std::println("  -h, --help\t- shows this help");
            exit(0);
        }
        else if (param == "--clean" || param == "-c")
        {
            clean_mode = true;
        }
        else if (param == "--jobs" || param == "-m")
        {
            if (i + 1 < argc)
            {
                try
                {
                    size_t num_jobs = std::stoull(argv[i + 1]);
                    set_parallel_jobs(num_jobs);
                    ++i;  // Skip the next argument since we consumed it
                }
                catch (const std::exception& e)
                {
                    trace_error(std::format("Invalid number of jobs: {}", argv[i + 1]));
                    exit(1);
                }
            }
            else
            {
                trace_error("--jobs/-m requires an argument");
                exit(1);
            }
        }
    }
}

void add_target_include_directories(Target& target, const std::vector<std::string_view>& include_dirs)
{
    for (const auto& dir : include_dirs)
    {
        target.compile_flags.push_back(std::format("-I{}", dir));
    }    
}

}  // namespace nobs
