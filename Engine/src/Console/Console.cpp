#include "Console.hpp"
#include "ConVar.hpp"
#include "ConCommand.hpp"
#include "Utils/Log.hpp"
#include <fstream>
#include <algorithm>
#include <chrono>

Console& Console::get()
{
    static Console instance;
    return instance;
}

void Console::initialize()
{
    if (m_initialized)
    {
        return;
    }

    m_initialized = true;
    LOG_ENGINE_INFO("Console initialized");
}

void Console::shutdown()
{
    m_initialized = false;
}

void Console::addLogEntry(const ConsoleLogEntry& entry)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    m_logEntries.push_back(entry);

    // Trim old entries
    while (m_logEntries.size() > m_maxLogEntries)
    {
        m_logEntries.pop_front();
    }
}

void Console::print(const std::string& msg)
{
    ConsoleLogEntry entry;
    entry.message = msg;
    entry.level = spdlog::level::info;
    entry.timestamp = getCurrentTimestamp();
    entry.source = "Console";
    addLogEntry(entry);
}

void Console::warning(const std::string& msg)
{
    ConsoleLogEntry entry;
    entry.message = msg;
    entry.level = spdlog::level::warn;
    entry.timestamp = getCurrentTimestamp();
    entry.source = "Console";
    addLogEntry(entry);
}

void Console::error(const std::string& msg)
{
    ConsoleLogEntry entry;
    entry.message = msg;
    entry.level = spdlog::level::err;
    entry.timestamp = getCurrentTimestamp();
    entry.source = "Console";
    addLogEntry(entry);
}

void Console::clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_logEntries.clear();
}

void Console::addToHistory(const std::string& command)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    // Don't add duplicates of the last command
    if (!m_commandHistory.empty() && m_commandHistory.front() == command)
    {
        return;
    }

    m_commandHistory.push_front(command);

    // Trim old entries
    while (m_commandHistory.size() > m_maxHistoryItems)
    {
        m_commandHistory.pop_back();
    }
}

const std::string& Console::getHistoryItem(int offset) const
{
    static const std::string s_empty;
    std::lock_guard<std::mutex> lock(m_mutex);

    if (offset < 0 || offset >= static_cast<int>(m_commandHistory.size()))
    {
        return s_empty;
    }

    return m_commandHistory[offset];
}

void Console::submitCommand(const std::string& command)
{
    if (command.empty())
    {
        return;
    }

    // Add to history
    addToHistory(command);

    // Print the command to console
    print("] {}", command);

    // Execute via CommandRegistry
    CommandRegistry::get().execute(command);
}

std::vector<std::string> Console::getCompletions(const std::string& partial)
{
    std::vector<std::string> results;

    if (partial.empty())
    {
        return results;
    }

    // Get matching cvars
    auto cvars = ConVarRegistry::get().findMatching(partial);
    for (auto* cvar : cvars)
    {
        results.push_back(cvar->getName());
    }

    // Get matching commands
    auto cmds = CommandRegistry::get().findMatching(partial);
    for (auto* cmd : cmds)
    {
        results.push_back(cmd->getName());
    }

    // Sort and remove duplicates
    std::sort(results.begin(), results.end());
    results.erase(std::unique(results.begin(), results.end()), results.end());

    return results;
}

void Console::execFile(const std::string& filename)
{
    std::string filepath = filename;

    // Add config/ prefix if not absolute path
    if (filepath.find(':') == std::string::npos && filepath[0] != '/' && filepath[0] != '\\')
    {
        if (filepath.find("config/") != 0 && filepath.find("config\\") != 0)
        {
            filepath = "config/" + filepath;
        }
    }

    // Add .cfg extension if missing
    if (filepath.find(".cfg") == std::string::npos)
    {
        filepath += ".cfg";
    }

    std::ifstream file(filepath);
    if (!file.is_open())
    {
        error("Could not open config file: " + filepath);
        return;
    }

    print("Executing {}", filepath);

    std::string line;
    int lineNum = 0;
    while (std::getline(file, line))
    {
        lineNum++;

        // Skip empty lines and comments
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos)
        {
            continue;
        }
        if (line[start] == '/' && line.size() > start + 1 && line[start + 1] == '/')
        {
            continue;
        }

        // Execute command
        CommandRegistry::get().execute(line);
    }
}

uint64_t Console::getCurrentTimestamp() const
{
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
}
