#pragma once

#include <string>
#include <vector>
#include <functional>
#include <unordered_map>
#include <mutex>

// Command argument parsing
struct CommandArgs
{
    std::string raw;                        // Original command string
    std::vector<std::string> args;          // Parsed arguments (args[0] = command name)

    size_t count() const { return args.size(); }

    const std::string& operator[](size_t index) const;

    int getInt(size_t index, int defaultVal = 0) const;
    float getFloat(size_t index, float defaultVal = 0.0f) const;
    const std::string& getString(size_t index, const std::string& defaultVal = "") const;

    // Join args from index to end
    std::string joinFrom(size_t startIndex) const;
};

// Command callback signature
using CommandCallback = std::function<void(const CommandArgs& args)>;

// Autocomplete callback - returns list of completions for partial argument
using AutocompleteCallback = std::function<std::vector<std::string>(const CommandArgs& args, size_t argIndex)>;

class ConCommand
{
private:
    std::string m_name;
    std::string m_description;
    uint32_t m_flags;
    CommandCallback m_callback;
    AutocompleteCallback m_autocomplete;

public:
    ConCommand(const std::string& name, CommandCallback callback,
               uint32_t flags, const std::string& description);

    void setAutocomplete(AutocompleteCallback callback);

    const std::string& getName() const { return m_name; }
    const std::string& getDescription() const { return m_description; }
    uint32_t getFlags() const { return m_flags; }

    void execute(const CommandArgs& args);
    std::vector<std::string> getCompletions(const CommandArgs& args, size_t argIndex);
};

// Command Registry (Singleton)
class CommandRegistry
{
public:
    static CommandRegistry& get();

    void registerCommand(ConCommand* cmd);
    void unregisterCommand(const std::string& name);

    ConCommand* find(const std::string& name);
    std::vector<ConCommand*> findMatching(const std::string& prefix);
    std::vector<ConCommand*> getAll();

    // Execute a command string (can be cvar assignment or command)
    bool execute(const std::string& commandLine);

    // Parse command line into args
    static CommandArgs parseCommandLine(const std::string& line);

private:
    CommandRegistry();
    std::unordered_map<std::string, ConCommand*> m_commands;
    mutable std::mutex m_mutex;

    void registerBuiltinCommands();
};

// Static registration helper
class ConCommandRegistrar
{
public:
    ConCommandRegistrar(ConCommand* cmd)
    {
        CommandRegistry::get().registerCommand(cmd);
    }
};

// Registration macro for commands
#define CONCOMMAND(name, callback, flags, description) \
    static EE::ConCommand g_cmd_##name(#name, callback, flags, description); \
    static EE::ConCommandRegistrar g_cmd_registrar_##name(&g_cmd_##name)
