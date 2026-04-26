#include "PIEProcessManager.hpp"
#include "Utils/Log.hpp"
#include <sstream>
#include <cerrno>
#include <cstring>

#ifdef _WIN32
// Windows process management — included via header
#else
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#endif

PIEProcessManager::~PIEProcessManager()
{
    killAll();
}

// Derive a working directory from a project file path. Returns the project's parent
// directory; if the path has no separator, returns "" so the caller falls back to the
// editor's current working directory rather than treating the bare filename as a dir.
static std::string deriveWorkingDir(const std::string& project_path)
{
    auto last_sep = project_path.find_last_of("/\\");
    if (last_sep == std::string::npos)
        return {};
    return project_path.substr(0, last_sep);
}

bool PIEProcessManager::spawnClient(int player_index,
                                     const std::string& game_exe_path,
                                     const std::string& project_path,
                                     const std::string& server_address,
                                     uint16_t server_port)
{
    std::vector<std::string> argv = {
        game_exe_path,
        "--project", project_path,
        "--connect", server_address,
        "--port",    std::to_string(server_port),
    };

    ProcessEntry entry{};
    entry.player_index = player_index;
    entry.label = "Player " + std::to_string(player_index);

    if (!launchProcess(game_exe_path, argv, deriveWorkingDir(project_path), entry))
    {
        LOG_ENGINE_ERROR("Failed to spawn client process for {}", entry.label);
        return false;
    }

    LOG_ENGINE_INFO("Spawned {} (PID {})", entry.label, entry.getPID());

    m_processes.push_back(std::move(entry));
    return true;
}

bool PIEProcessManager::spawnServer(const std::string& server_exe_path,
                                     const std::string& project_path,
                                     uint16_t port)
{
    std::vector<std::string> argv = {
        server_exe_path,
        "--project", project_path,
        "--port",    std::to_string(port),
    };

    ProcessEntry entry{};
    entry.player_index = 0; // 0 = server
    entry.label = "Dedicated Server";

    if (!launchProcess(server_exe_path, argv, deriveWorkingDir(project_path), entry))
    {
        LOG_ENGINE_ERROR("Failed to spawn dedicated server process");
        return false;
    }

    LOG_ENGINE_INFO("Spawned Dedicated Server (PID {})", entry.getPID());

    m_processes.push_back(std::move(entry));
    return true;
}

void PIEProcessManager::killAll()
{
    for (auto& proc : m_processes)
    {
        if (proc.isRunning())
        {
            LOG_ENGINE_INFO("Terminating PIE process: {}", proc.label);
            proc.terminate();
        }
        proc.closeHandles();
    }
    m_processes.clear();
}

int PIEProcessManager::countRunning() const
{
    int count = 0;
    for (const auto& proc : m_processes)
    {
        if (proc.isRunning())
            count++;
    }
    return count;
}

// ---------- Platform-specific implementations ----------

#ifdef _WIN32

uint32_t PIEProcessManager::ProcessEntry::getPID() const
{
    return static_cast<uint32_t>(process_id);
}

bool PIEProcessManager::ProcessEntry::isRunning() const
{
    if (!process_handle) return false;
    DWORD exit_code = 0;
    if (!GetExitCodeProcess(process_handle, &exit_code))
        return false;
    return exit_code == STILL_ACTIVE;
}

void PIEProcessManager::ProcessEntry::terminate()
{
    if (process_handle)
        TerminateProcess(process_handle, 0);
}

void PIEProcessManager::ProcessEntry::closeHandles()
{
    if (process_handle)
    {
        CloseHandle(process_handle);
        process_handle = nullptr;
    }
}

// Quote a single argv token for CreateProcessA. Implements the algorithm documented at
// https://docs.microsoft.com/en-us/archive/blogs/twistylittlepassagesallalike/everyone-quotes-command-line-arguments-the-wrong-way
static std::string quoteWindowsArg(const std::string& arg)
{
    if (!arg.empty() &&
        arg.find_first_of(" \t\n\v\"") == std::string::npos)
    {
        return arg;
    }

    std::string out;
    out.push_back('"');
    for (auto it = arg.begin(); ; ++it)
    {
        unsigned backslashes = 0;
        while (it != arg.end() && *it == '\\')
        {
            ++backslashes;
            ++it;
        }
        if (it == arg.end())
        {
            out.append(backslashes * 2, '\\');
            break;
        }
        if (*it == '"')
        {
            out.append(backslashes * 2 + 1, '\\');
            out.push_back('"');
        }
        else
        {
            out.append(backslashes, '\\');
            out.push_back(*it);
        }
    }
    out.push_back('"');
    return out;
}

bool PIEProcessManager::launchProcess(const std::string& exe_path,
                                       const std::vector<std::string>& argv,
                                       const std::string& working_dir, ProcessEntry& entry)
{
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    // Build a properly quoted command line from argv. CreateProcessA requires a mutable
    // buffer.
    std::string cmd;
    for (size_t i = 0; i < argv.size(); ++i)
    {
        if (i > 0) cmd.push_back(' ');
        cmd += quoteWindowsArg(argv[i]);
    }

    if (!CreateProcessA(
            exe_path.c_str(),
            cmd.data(),
            nullptr, nullptr,
            FALSE,
            0,
            nullptr,
            working_dir.empty() ? nullptr : working_dir.c_str(),
            &si, &pi))
    {
        LOG_ENGINE_ERROR("CreateProcess failed (error {}): {}", GetLastError(), exe_path);
        return false;
    }

    entry.process_handle = pi.hProcess;
    entry.process_id = pi.dwProcessId;

    // We don't need the thread handle
    CloseHandle(pi.hThread);
    return true;
}

#else // POSIX (Linux + macOS)

uint32_t PIEProcessManager::ProcessEntry::getPID() const
{
    return static_cast<uint32_t>(pid);
}

bool PIEProcessManager::ProcessEntry::isRunning() const
{
    if (pid <= 0) return false;
    int status = 0;
    pid_t result = waitpid(pid, &status, WNOHANG);
    if (result == 0) return true;  // still running
    if (result < 0) return false;  // error (already reaped or invalid)
    return false;                  // exited
}

void PIEProcessManager::ProcessEntry::terminate()
{
    if (pid <= 0) return;

    // Try graceful shutdown first
    ::kill(pid, SIGTERM);

    // Wait briefly, then force kill if still alive
    for (int i = 0; i < 10; i++)
    {
        usleep(50000); // 50ms
        int status = 0;
        pid_t result = waitpid(pid, &status, WNOHANG);
        if (result != 0) return; // exited or error
    }

    // Force kill
    ::kill(pid, SIGKILL);
    int status = 0;
    waitpid(pid, &status, 0); // reap
}

void PIEProcessManager::ProcessEntry::closeHandles()
{
    if (pid > 0)
    {
        int status = 0;
        waitpid(pid, &status, WNOHANG); // reap zombie if not already reaped
        pid = 0;
    }
}

bool PIEProcessManager::launchProcess(const std::string& exe_path,
                                       const std::vector<std::string>& argv,
                                       const std::string& working_dir, ProcessEntry& entry)
{
    // Self-pipe with O_CLOEXEC so the parent can distinguish a successful exec (pipe
    // closes empty) from a failed chdir/exec in the child (errno written to pipe).
    int err_pipe[2];
    if (pipe(err_pipe) != 0)
    {
        LOG_ENGINE_ERROR("pipe() failed for: {}", exe_path);
        return false;
    }
    if (fcntl(err_pipe[1], F_SETFD, FD_CLOEXEC) == -1)
    {
        ::close(err_pipe[0]);
        ::close(err_pipe[1]);
        LOG_ENGINE_ERROR("fcntl(FD_CLOEXEC) failed for: {}", exe_path);
        return false;
    }

    pid_t child = fork();
    if (child < 0)
    {
        ::close(err_pipe[0]);
        ::close(err_pipe[1]);
        LOG_ENGINE_ERROR("fork() failed for: {}", exe_path);
        return false;
    }

    if (child == 0)
    {
        // Child: report the first failing errno through err_pipe[1] before _exit.
        ::close(err_pipe[0]);

        auto report_and_exit = [&](int err) {
            (void)!::write(err_pipe[1], &err, sizeof(err));
            ::close(err_pipe[1]);
            _exit(127);
        };

        if (!working_dir.empty() && chdir(working_dir.c_str()) != 0)
            report_and_exit(errno);

        // Build a NULL-terminated argv array. Each entry points into a string we own
        // until execv replaces our address space, so the c_str() pointers stay valid.
        std::vector<char*> raw_argv;
        raw_argv.reserve(argv.size() + 1);
        for (const auto& s : argv)
            raw_argv.push_back(const_cast<char*>(s.c_str()));
        raw_argv.push_back(nullptr);

        execv(exe_path.c_str(), raw_argv.data());
        // execv only returns on failure
        report_and_exit(errno);
    }

    // Parent
    ::close(err_pipe[1]);

    int child_errno = 0;
    ssize_t n;
    do {
        n = ::read(err_pipe[0], &child_errno, sizeof(child_errno));
    } while (n == -1 && errno == EINTR);
    ::close(err_pipe[0]);

    if (n > 0)
    {
        // Reap the failed child so it doesn't linger as a zombie.
        int status = 0;
        waitpid(child, &status, 0);
        LOG_ENGINE_ERROR("Child failed to start ({}): {}",
            std::strerror(child_errno), exe_path);
        return false;
    }

    entry.pid = child;
    return true;
}

#endif
