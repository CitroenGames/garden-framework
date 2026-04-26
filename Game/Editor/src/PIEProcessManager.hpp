#pragma once

#include <vector>
#include <string>
#include <cstdint>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/types.h>
#endif

class PIEProcessManager
{
public:
    ~PIEProcessManager();

    // Spawn a game client connecting to the given server.
    bool spawnClient(int player_index,
                     const std::string& game_exe_path,
                     const std::string& project_path,
                     const std::string& server_address,
                     uint16_t server_port);

    // Spawn a dedicated server process.
    bool spawnServer(const std::string& server_exe_path,
                     const std::string& project_path,
                     uint16_t port);

    // Terminate all spawned processes.
    void killAll();

    // Return how many spawned processes are still running.
    int countRunning() const;

    bool hasProcesses() const { return !m_processes.empty(); }

private:
    struct ProcessEntry
    {
        int         player_index; // 0 = server, 2+ = client players
        std::string label;
#ifdef _WIN32
        HANDLE      process_handle = nullptr;
        DWORD       process_id     = 0;
#else
        pid_t       pid            = 0;
#endif

        // Cross-platform PID accessor for logging
        uint32_t getPID() const;

        bool isRunning() const;
        void terminate();
        void closeHandles();
    };

    std::vector<ProcessEntry> m_processes;

    // argv[0] should be the executable name (or path); the platform implementation will
    // either exec it directly (POSIX) or build a quoted command line from it (Windows).
    bool launchProcess(const std::string& exe_path,
                       const std::vector<std::string>& argv,
                       const std::string& working_dir, ProcessEntry& entry);
};
