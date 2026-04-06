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
    loadHistory("config/console_history.txt");
    LOG_ENGINE_INFO("Console initialized");
}

void Console::shutdown()
{
    saveHistory("config/console_history.txt");
    m_initialized = false;
}

void Console::addLogEntry(const ConsoleLogEntry& entry)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    m_logEntries.push_back(entry);
    incrementCounter(entry.level);

    // Trim old entries
    while (m_logEntries.size() > m_maxLogEntries)
    {
        decrementCounter(m_logEntries.front().level);
        m_logEntries.pop_front();
    }
}

void Console::incrementCounter(spdlog::level::level_enum level)
{
    if (level >= spdlog::level::err)
        m_errorCount++;
    else if (level == spdlog::level::warn)
        m_warnCount++;
    else
        m_infoCount++;
}

void Console::decrementCounter(spdlog::level::level_enum level)
{
    if (level >= spdlog::level::err)
        m_errorCount = std::max(0, m_errorCount - 1);
    else if (level == spdlog::level::warn)
        m_warnCount = std::max(0, m_warnCount - 1);
    else
        m_infoCount = std::max(0, m_infoCount - 1);
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
    m_infoCount = 0;
    m_warnCount = 0;
    m_errorCount = 0;
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

    // Echo the command with distinct type for styling
    ConsoleLogEntry echo;
    echo.message = command;
    echo.level = spdlog::level::info;
    echo.timestamp = getCurrentTimestamp();
    echo.source = "Console";
    echo.type = ConsoleEntryType::CommandEcho;
    addLogEntry(echo);

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

void Console::saveHistory(const std::string& filepath)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    std::ofstream file(filepath);
    if (!file.is_open())
    {
        return;
    }

    for (const auto& cmd : m_commandHistory)
    {
        file << cmd << "\n";
    }
}

void Console::loadHistory(const std::string& filepath)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    std::ifstream file(filepath);
    if (!file.is_open())
    {
        return;
    }

    m_commandHistory.clear();
    std::string line;
    while (std::getline(file, line))
    {
        if (!line.empty())
        {
            m_commandHistory.push_back(line);
        }

        if (m_commandHistory.size() >= m_maxHistoryItems)
        {
            break;
        }
    }
}

uint64_t Console::getCurrentTimestamp() const
{
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
}
