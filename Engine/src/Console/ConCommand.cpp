#include "ConCommand.hpp"
#include "ConVar.hpp"
#include "Console.hpp"
#include "Utils/Log.hpp"
#include <algorithm>
#include <sstream>
#include <SDL.h>

// CommandArgs implementation
static const std::string s_emptyString;

const std::string& CommandArgs::operator[](size_t index) const
{
    if (index < args.size())
    {
        return args[index];
    }
    return s_emptyString;
}

int CommandArgs::getInt(size_t index, int defaultVal) const
{
    if (index >= args.size())
    {
        return defaultVal;
    }
    try
    {
        return std::stoi(args[index]);
    }
    catch (...)
    {
        return defaultVal;
    }
}

float CommandArgs::getFloat(size_t index, float defaultVal) const
{
    if (index >= args.size())
    {
        return defaultVal;
    }
    try
    {
        return std::stof(args[index]);
    }
    catch (...)
    {
        return defaultVal;
    }
}

const std::string& CommandArgs::getString(size_t index, const std::string& defaultVal) const
{
    if (index >= args.size())
    {
        return defaultVal;
    }
    return args[index];
}

std::string CommandArgs::joinFrom(size_t startIndex) const
{
    if (startIndex >= args.size())
    {
        return "";
    }

    std::string result;
    for (size_t i = startIndex; i < args.size(); ++i)
    {
        if (i > startIndex)
        {
            result += " ";
        }
        result += args[i];
    }
    return result;
}

// ConCommand implementation
ConCommand::ConCommand(const std::string& name, CommandCallback callback,
                       uint32_t flags, const std::string& description)
    : m_name(name)
    , m_description(description)
    , m_flags(flags)
    , m_callback(std::move(callback))
{
}

void ConCommand::setAutocomplete(AutocompleteCallback callback)
{
    m_autocomplete = std::move(callback);
}

void ConCommand::execute(const CommandArgs& args)
{
    if (m_callback)
    {
        m_callback(args);
    }
}

std::vector<std::string> ConCommand::getCompletions(const CommandArgs& args, size_t argIndex)
{
    if (m_autocomplete)
    {
        return m_autocomplete(args, argIndex);
    }
    return {};
}

// CommandRegistry implementation
CommandRegistry& CommandRegistry::get()
{
    static CommandRegistry instance;
    return instance;
}

CommandRegistry::CommandRegistry()
{
    registerBuiltinCommands();
}

void CommandRegistry::registerCommand(ConCommand* cmd)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_commands.find(cmd->getName()) != m_commands.end())
    {
        LOG_ENGINE_WARN("Command '{}' already registered, ignoring duplicate", cmd->getName());
        return;
    }

    m_commands[cmd->getName()] = cmd;
    LOG_ENGINE_TRACE("Registered command: {}", cmd->getName());
}

void CommandRegistry::unregisterCommand(const std::string& name)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_commands.erase(name);
}

ConCommand* CommandRegistry::find(const std::string& name)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_commands.find(name);
    return (it != m_commands.end()) ? it->second : nullptr;
}

std::vector<ConCommand*> CommandRegistry::findMatching(const std::string& prefix)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<ConCommand*> result;

    std::string lowerPrefix = prefix;
    std::transform(lowerPrefix.begin(), lowerPrefix.end(), lowerPrefix.begin(), ::tolower);

    for (auto& [name, cmd] : m_commands)
    {
        std::string lowerName = name;
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);

        if (lowerName.find(lowerPrefix) != std::string::npos)
        {
            result.push_back(cmd);
        }
    }

    std::sort(result.begin(), result.end(), [](ConCommand* a, ConCommand* b) {
        return a->getName() < b->getName();
    });

    return result;
}

std::vector<ConCommand*> CommandRegistry::getAll()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<ConCommand*> result;
    result.reserve(m_commands.size());

    for (auto& [name, cmd] : m_commands)
    {
        result.push_back(cmd);
    }

    std::sort(result.begin(), result.end(), [](ConCommand* a, ConCommand* b) {
        return a->getName() < b->getName();
    });

    return result;
}

CommandArgs CommandRegistry::parseCommandLine(const std::string& line)
{
    CommandArgs args;
    args.raw = line;

    std::string current;
    bool inQuotes = false;

    for (size_t i = 0; i < line.size(); ++i)
    {
        char c = line[i];

        if (c == '"')
        {
            inQuotes = !inQuotes;
        }
        else if ((c == ' ' || c == '\t') && !inQuotes)
        {
            if (!current.empty())
            {
                args.args.push_back(current);
                current.clear();
            }
        }
        else
        {
            current += c;
        }
    }

    if (!current.empty())
    {
        args.args.push_back(current);
    }

    return args;
}

bool CommandRegistry::execute(const std::string& commandLine)
{
    // Skip empty lines and comments
    std::string trimmed = commandLine;
    size_t start = trimmed.find_first_not_of(" \t");
    if (start == std::string::npos)
    {
        return true;  // Empty line is not an error
    }
    trimmed = trimmed.substr(start);

    if (trimmed.empty() || (trimmed[0] == '/' && trimmed.size() > 1 && trimmed[1] == '/'))
    {
        return true;  // Comment
    }

    CommandArgs args = parseCommandLine(trimmed);
    if (args.args.empty())
    {
        return true;
    }

    const std::string& name = args.args[0];

    // First, check if it's a command
    ConCommand* cmd = find(name);
    if (cmd)
    {
        cmd->execute(args);
        return true;
    }

    // Second, check if it's a cvar
    ConVarBase* cvar = ConVarRegistry::get().find(name);
    if (cvar)
    {
        if (args.args.size() == 1)
        {
            // Just print the value
            Console::get().print("{} = \"{}\" (default: \"{}\")",
                                 cvar->getName(), cvar->getValueString(), cvar->getDefaultValueString());
            Console::get().print("  {}", cvar->getDescription());
        }
        else
        {
            // Set the value
            std::string value = args.joinFrom(1);
            if (!cvar->setFromString(value))
            {
                Console::get().print("Failed to set {} to '{}'", name, value);
            }
        }
        return true;
    }

    Console::get().print("Unknown command or cvar: {}", name);
    return false;
}

void CommandRegistry::registerBuiltinCommands()
{
    // help [command] - Show help for command or list all
    static ConCommand helpCmd("help", [](const CommandArgs& args) {
        if (args.count() > 1)
        {
            const std::string& name = args[1];

            // Check commands
            ConCommand* cmd = CommandRegistry::get().find(name);
            if (cmd)
            {
                Console::get().print("{} - {}", cmd->getName(), cmd->getDescription());
                return;
            }

            // Check cvars
            ConVarBase* cvar = ConVarRegistry::get().find(name);
            if (cvar)
            {
                Console::get().print("{} = \"{}\" (default: \"{}\")",
                                     cvar->getName(), cvar->getValueString(), cvar->getDefaultValueString());
                Console::get().print("  {}", cvar->getDescription());
                if (cvar->hasBounds())
                {
                    Console::get().print("  Range: [{}, {}]", cvar->getMinBound(), cvar->getMaxBound());
                }
                return;
            }

            Console::get().print("Unknown command or cvar: {}", name);
        }
        else
        {
            Console::get().print("Commands:");
            auto commands = CommandRegistry::get().getAll();
            for (auto* cmd : commands)
            {
                Console::get().print("  {} - {}", cmd->getName(), cmd->getDescription());
            }
            Console::get().print("");
            Console::get().print("Use 'find <pattern>' to search cvars and commands");
        }
    }, 0, "Display help information");
    m_commands["help"] = &helpCmd;

    // find <pattern> - Search cvars and commands
    static ConCommand findCmd("find", [](const CommandArgs& args) {
        if (args.count() < 2)
        {
            Console::get().print("Usage: find <pattern>");
            return;
        }

        const std::string& pattern = args[1];

        auto cvars = ConVarRegistry::get().findMatching(pattern);
        auto cmds = CommandRegistry::get().findMatching(pattern);

        if (cvars.empty() && cmds.empty())
        {
            Console::get().print("No matches found for '{}'", pattern);
            return;
        }

        for (auto* cvar : cvars)
        {
            Console::get().print("{} = \"{}\"", cvar->getName(), cvar->getValueString());
        }
        for (auto* cmd : cmds)
        {
            Console::get().print("{} (command)", cmd->getName());
        }
    }, 0, "Find commands and cvars by name");
    m_commands["find"] = &findCmd;

    // clear - Clear console
    static ConCommand clearCmd("clear", [](const CommandArgs&) {
        Console::get().clear();
    }, 0, "Clear console output");
    m_commands["clear"] = &clearCmd;

    // quit / exit - Exit application
    static ConCommand quitCmd("quit", [](const CommandArgs&) {
        SDL_Event quit_event;
        quit_event.type = SDL_QUIT;
        SDL_PushEvent(&quit_event);
    }, 0, "Exit the application");
    m_commands["quit"] = &quitCmd;

    static ConCommand exitCmd("exit", [](const CommandArgs&) {
        SDL_Event quit_event;
        quit_event.type = SDL_QUIT;
        SDL_PushEvent(&quit_event);
    }, 0, "Exit the application");
    m_commands["exit"] = &exitCmd;

    // echo <text> - Print text to console
    static ConCommand echoCmd("echo", [](const CommandArgs& args) {
        Console::get().print("{}", args.joinFrom(1));
    }, 0, "Print text to console");
    m_commands["echo"] = &echoCmd;

    // exec <filename> - Execute config file
    static ConCommand execCmd("exec", [](const CommandArgs& args) {
        if (args.count() < 2)
        {
            Console::get().print("Usage: exec <filename>");
            return;
        }
        Console::get().execFile(args[1]);
    }, 0, "Execute a config file");
    m_commands["exec"] = &execCmd;

    // cvarlist - List all cvars
    static ConCommand cvarlistCmd("cvarlist", [](const CommandArgs&) {
        auto cvars = ConVarRegistry::get().getAll();
        Console::get().print("{} cvars:", cvars.size());
        for (auto* cvar : cvars)
        {
            Console::get().print("  {} = \"{}\"", cvar->getName(), cvar->getValueString());
        }
    }, 0, "List all cvars");
    m_commands["cvarlist"] = &cvarlistCmd;

    // cmdlist - List all commands
    static ConCommand cmdlistCmd("cmdlist", [](const CommandArgs&) {
        auto cmds = CommandRegistry::get().getAll();
        Console::get().print("{} commands:", cmds.size());
        for (auto* cmd : cmds)
        {
            Console::get().print("  {}", cmd->getName());
        }
    }, 0, "List all commands");
    m_commands["cmdlist"] = &cmdlistCmd;

    // reset <cvar> - Reset cvar to default
    static ConCommand resetCmd("reset", [](const CommandArgs& args) {
        if (args.count() < 2)
        {
            Console::get().print("Usage: reset <cvar>");
            return;
        }

        ConVarBase* cvar = ConVarRegistry::get().find(args[1]);
        if (cvar)
        {
            cvar->reset();
            Console::get().print("{} reset to \"{}\"", cvar->getName(), cvar->getValueString());
        }
        else
        {
            Console::get().print("Unknown cvar: {}", args[1]);
        }
    }, 0, "Reset a cvar to its default value");
    m_commands["reset"] = &resetCmd;
}