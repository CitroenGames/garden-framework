#include "PIEProcessManager.hpp"
#include "Utils/Log.hpp"
#include <sstream>

#ifdef _WIN32
// Windows process management — included via header
#else
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

PIEProcessManager::~PIEProcessManager()
{
    killAll();
}

bool PIEProcessManager::spawnClient(int player_index,
                                     const std::string& game_exe_path,
                                     const std::string& project_path,
                                     const std::string& server_address,
                                     uint16_t server_port)
{
    std::ostringstream args;
    args << "\"" << game_exe_path << "\""
         << " --project \"" << project_path << "\""
         << " --connect " << server_address
         << " --port " << server_port;

    // Extract working directory from project path
    std::string work_dir = project_path;
    auto last_sep = work_dir.find_last_of("/\\");
    if (last_sep != std::string::npos)
        work_dir = work_dir.substr(0, last_sep);

    ProcessEntry entry{};
    entry.player_index = player_index;
    entry.label = "Player " + std::to_string(player_index);

    if (!launchProcess(game_exe_path, args.str(), work_dir, entry))
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
    std::ostringstream args;
    args << "\"" << server_exe_path << "\""
         << " --project \"" << project_path << "\""
         << " --port " << port;

    std::string work_dir = project_path;
    auto last_sep = work_dir.find_last_of("/\\");
    if (last_sep != std::string::npos)
        work_dir = work_dir.substr(0, last_sep);

    ProcessEntry entry{};
    entry.player_index = 0; // 0 = server
    entry.label = "Dedicated Server";

    if (!launchProcess(server_exe_path, args.str(), work_dir, entry))
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

bool PIEProcessManager::launchProcess(const std::string& exe_path, const std::string& args,
                                       const std::string& working_dir, ProcessEntry& entry)
{
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    // CreateProcessA needs a mutable command line buffer
    std::string cmd = args;

    if (!CreateProcessA(
            nullptr,
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

bool PIEProcessManager::launchProcess(const std::string& exe_path, const std::string& args,
                                       const std::string& working_dir, ProcessEntry& entry)
{
    pid_t child = fork();
    if (child < 0)
    {
        LOG_ENGINE_ERROR("fork() failed for: {}", exe_path);
        return false;
    }

    if (child == 0)
    {
        // Child process
        if (!working_dir.empty())
        {
            if (chdir(working_dir.c_str()) != 0)
                _exit(126);
        }
        execl("/bin/sh", "sh", "-c", args.c_str(), nullptr);
        _exit(127); // exec failed
    }

    entry.pid = child;
    return true;
}

#endif
