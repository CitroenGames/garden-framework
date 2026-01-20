#pragma once

#include <string>
#include <deque>
#include <mutex>
#include <cstdint>
#include "spdlog/spdlog.h"

// Log entry with level and timestamp
struct ConsoleLogEntry
{
    std::string message;
    spdlog::level::level_enum level = spdlog::level::info;
    uint64_t timestamp = 0;
    std::string source;  // "Engine", "Client", "LUA", etc.
};

class Console
{
public:
    static Console& get();

    // Initialize console (call after CLog::Init)
    void initialize();
    void shutdown();

    // Logging - adds entry to log buffer
    void addLogEntry(const ConsoleLogEntry& entry);

    // Direct printing (for command output) - uses info level
    template<typename... Args>
    void print(const std::string& fmt, Args&&... args)
    {
        try
        {
            std::string formatted = fmt::format(fmt::runtime(fmt), std::forward<Args>(args)...);
            ConsoleLogEntry entry;
            entry.message = formatted;
            entry.level = spdlog::level::info;
            entry.timestamp = getCurrentTimestamp();
            entry.source = "Console";
            addLogEntry(entry);
        }
        catch (const fmt::format_error& e)
        {
            ConsoleLogEntry entry;
            entry.message = fmt;
            entry.level = spdlog::level::info;
            entry.timestamp = getCurrentTimestamp();
            entry.source = "Console";
            addLogEntry(entry);
        }
    }

    // Print with explicit level
    void print(const std::string& msg);
    void warning(const std::string& msg);
    void error(const std::string& msg);

    // History
    const std::deque<ConsoleLogEntry>& getLogEntries() const { return m_logEntries; }
    void clear();

    // Command history
    void addToHistory(const std::string& command);
    const std::string& getHistoryItem(int offset) const;
    int getHistoryCount() const { return static_cast<int>(m_commandHistory.size()); }

    // Input handling
    void submitCommand(const std::string& command);
    std::vector<std::string> getCompletions(const std::string& partial);

    // Config file execution
    void execFile(const std::string& filename);

    // Settings
    void setMaxLogEntries(size_t max) { m_maxLogEntries = max; }
    size_t getMaxLogEntries() const { return m_maxLogEntries; }

private:
    Console() = default;
    ~Console() = default;

    uint64_t getCurrentTimestamp() const;

    std::deque<ConsoleLogEntry> m_logEntries;
    std::deque<std::string> m_commandHistory;
    size_t m_maxLogEntries = 1000;
    size_t m_maxHistoryItems = 100;
    mutable std::mutex m_mutex;
    bool m_initialized = false;
};