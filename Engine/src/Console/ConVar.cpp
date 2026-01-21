#include "ConVar.hpp"
#include "Utils/Log.hpp"
#include <algorithm>
#include <fstream>
#include <sstream>
#include <cctype>

// ConVarBase constructors
ConVarBase::ConVarBase(const std::string& name, int defaultValue, uint32_t flags, const std::string& description)
    : m_name(name)
    , m_description(description)
    , m_flags(flags)
    , m_defaultValue(defaultValue)
    , m_currentValue(defaultValue)
{
}

ConVarBase::ConVarBase(const std::string& name, float defaultValue, uint32_t flags, const std::string& description)
    : m_name(name)
    , m_description(description)
    , m_flags(flags)
    , m_defaultValue(defaultValue)
    , m_currentValue(defaultValue)
{
}

ConVarBase::ConVarBase(const std::string& name, bool defaultValue, uint32_t flags, const std::string& description)
    : m_name(name)
    , m_description(description)
    , m_flags(flags)
    , m_defaultValue(defaultValue)
    , m_currentValue(defaultValue)
{
}

ConVarBase::ConVarBase(const std::string& name, const char* defaultValue, uint32_t flags, const std::string& description)
    : m_name(name)
    , m_description(description)
    , m_flags(flags)
    , m_defaultValue(std::string(defaultValue))
    , m_currentValue(std::string(defaultValue))
{
}

ConVarBase::ConVarBase(const std::string& name, const std::string& defaultValue, uint32_t flags, const std::string& description)
    : m_name(name)
    , m_description(description)
    , m_flags(flags)
    , m_defaultValue(defaultValue)
    , m_currentValue(defaultValue)
{
}

// Value getters with type conversion
int ConVarBase::getInt() const
{
    if (std::holds_alternative<int>(m_currentValue))
    {
        return std::get<int>(m_currentValue);
    }
    else if (std::holds_alternative<float>(m_currentValue))
    {
        return static_cast<int>(std::get<float>(m_currentValue));
    }
    else if (std::holds_alternative<bool>(m_currentValue))
    {
        return std::get<bool>(m_currentValue) ? 1 : 0;
    }
    else if (std::holds_alternative<std::string>(m_currentValue))
    {
        try
        {
            return std::stoi(std::get<std::string>(m_currentValue));
        }
        catch (...)
        {
            return 0;
        }
    }
    return 0;
}

float ConVarBase::getFloat() const
{
    if (std::holds_alternative<float>(m_currentValue))
    {
        return std::get<float>(m_currentValue);
    }
    else if (std::holds_alternative<int>(m_currentValue))
    {
        return static_cast<float>(std::get<int>(m_currentValue));
    }
    else if (std::holds_alternative<bool>(m_currentValue))
    {
        return std::get<bool>(m_currentValue) ? 1.0f : 0.0f;
    }
    else if (std::holds_alternative<std::string>(m_currentValue))
    {
        try
        {
            return std::stof(std::get<std::string>(m_currentValue));
        }
        catch (...)
        {
            return 0.0f;
        }
    }
    return 0.0f;
}

bool ConVarBase::getBool() const
{
    if (std::holds_alternative<bool>(m_currentValue))
    {
        return std::get<bool>(m_currentValue);
    }
    else if (std::holds_alternative<int>(m_currentValue))
    {
        return std::get<int>(m_currentValue) != 0;
    }
    else if (std::holds_alternative<float>(m_currentValue))
    {
        return std::get<float>(m_currentValue) != 0.0f;
    }
    else if (std::holds_alternative<std::string>(m_currentValue))
    {
        const std::string& s = std::get<std::string>(m_currentValue);
        return !s.empty() && s != "0" && s != "false";
    }
    return false;
}

const std::string& ConVarBase::getString() const
{
    static std::string s_empty;
    if (std::holds_alternative<std::string>(m_currentValue))
    {
        return std::get<std::string>(m_currentValue);
    }
    return s_empty;
}

// Value setters
bool ConVarBase::setInt(int value)
{
    if (!validateCheatRestriction())
    {
        return false;
    }

    ConVarValue newValue = value;
    if (!validateAndClamp(newValue))
    {
        return false;
    }

    ConVarValue oldValue = m_currentValue;
    m_currentValue = newValue;
    notifyCallbacks(oldValue);
    return true;
}

bool ConVarBase::setFloat(float value)
{
    if (!validateCheatRestriction())
    {
        return false;
    }

    ConVarValue newValue = value;
    if (!validateAndClamp(newValue))
    {
        return false;
    }

    ConVarValue oldValue = m_currentValue;
    m_currentValue = newValue;
    notifyCallbacks(oldValue);
    return true;
}

bool ConVarBase::setBool(bool value)
{
    if (!validateCheatRestriction())
    {
        return false;
    }

    ConVarValue oldValue = m_currentValue;
    m_currentValue = value;
    notifyCallbacks(oldValue);
    return true;
}

bool ConVarBase::setString(const std::string& value)
{
    if (!validateCheatRestriction())
    {
        return false;
    }

    ConVarValue oldValue = m_currentValue;
    m_currentValue = value;
    notifyCallbacks(oldValue);
    return true;
}

bool ConVarBase::setFromString(const std::string& valueStr)
{
    if (!validateCheatRestriction())
    {
        return false;
    }

    ConVarValue oldValue = m_currentValue;

    // Try to set based on the current type
    if (std::holds_alternative<int>(m_defaultValue))
    {
        try
        {
            int val = std::stoi(valueStr);
            ConVarValue newValue = val;
            if (!validateAndClamp(newValue))
            {
                return false;
            }
            m_currentValue = newValue;
        }
        catch (...)
        {
            LOG_ENGINE_WARN("ConVar {}: Invalid integer value '{}'", m_name, valueStr);
            return false;
        }
    }
    else if (std::holds_alternative<float>(m_defaultValue))
    {
        try
        {
            float val = std::stof(valueStr);
            ConVarValue newValue = val;
            if (!validateAndClamp(newValue))
            {
                return false;
            }
            m_currentValue = newValue;
        }
        catch (...)
        {
            LOG_ENGINE_WARN("ConVar {}: Invalid float value '{}'", m_name, valueStr);
            return false;
        }
    }
    else if (std::holds_alternative<bool>(m_defaultValue))
    {
        std::string lower = valueStr;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        bool val = (lower == "1" || lower == "true" || lower == "yes" || lower == "on");
        m_currentValue = val;
    }
    else
    {
        m_currentValue = valueStr;
    }

    notifyCallbacks(oldValue);
    return true;
}

void ConVarBase::reset()
{
    ConVarValue oldValue = m_currentValue;
    m_currentValue = m_defaultValue;
    notifyCallbacks(oldValue);
}

void ConVarBase::setBounds(float min, float max)
{
    m_minValue = min;
    m_maxValue = max;
}

void ConVarBase::addChangeCallback(ConVarCallback callback)
{
    m_callbacks.push_back(std::move(callback));
}

std::string ConVarBase::getValueString() const
{
    if (std::holds_alternative<int>(m_currentValue))
    {
        return std::to_string(std::get<int>(m_currentValue));
    }
    else if (std::holds_alternative<float>(m_currentValue))
    {
        return std::to_string(std::get<float>(m_currentValue));
    }
    else if (std::holds_alternative<bool>(m_currentValue))
    {
        return std::get<bool>(m_currentValue) ? "1" : "0";
    }
    else if (std::holds_alternative<std::string>(m_currentValue))
    {
        return std::get<std::string>(m_currentValue);
    }
    return "";
}

std::string ConVarBase::getDefaultValueString() const
{
    if (std::holds_alternative<int>(m_defaultValue))
    {
        return std::to_string(std::get<int>(m_defaultValue));
    }
    else if (std::holds_alternative<float>(m_defaultValue))
    {
        return std::to_string(std::get<float>(m_defaultValue));
    }
    else if (std::holds_alternative<bool>(m_defaultValue))
    {
        return std::get<bool>(m_defaultValue) ? "1" : "0";
    }
    else if (std::holds_alternative<std::string>(m_defaultValue))
    {
        return std::get<std::string>(m_defaultValue);
    }
    return "";
}

void ConVarBase::notifyCallbacks(const ConVarValue& oldValue)
{
    for (auto& callback : m_callbacks)
    {
        callback(this, oldValue, m_currentValue);
    }
}

bool ConVarBase::validateAndClamp(ConVarValue& value)
{
    if (!hasBounds())
    {
        return true;
    }

    if (std::holds_alternative<int>(value))
    {
        int& v = std::get<int>(value);
        v = std::clamp(v, static_cast<int>(m_minValue.value()), static_cast<int>(m_maxValue.value()));
    }
    else if (std::holds_alternative<float>(value))
    {
        float& v = std::get<float>(value);
        v = std::clamp(v, m_minValue.value(), m_maxValue.value());
    }

    return true;
}

bool ConVarBase::validateCheatRestriction() const
{
    if (!hasFlag(ConVarFlags::CHEAT))
    {
        return true;  // Not a cheat cvar, always allowed
    }

    // Check sv_cheats
    ConVarBase* sv_cheats = ConVarRegistry::get().find("sv_cheats");
    if (!sv_cheats || sv_cheats->getInt() == 0)
    {
        LOG_ENGINE_WARN("{} is cheat protected", m_name);
        return false;
    }

    return true;
}

// ConVarRegistry implementation
ConVarRegistry& ConVarRegistry::get()
{
    static ConVarRegistry instance;
    return instance;
}

void ConVarRegistry::registerConVar(ConVarBase* cvar)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_cvars.find(cvar->getName()) != m_cvars.end())
    {
        return;  // Duplicate - can't log here, called during static init
    }

    m_cvars[cvar->getName()] = cvar;
}

void ConVarRegistry::unregisterConVar(const std::string& name)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cvars.erase(name);
}

ConVarBase* ConVarRegistry::find(const std::string& name)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_cvars.find(name);
    return (it != m_cvars.end()) ? it->second : nullptr;
}

std::vector<ConVarBase*> ConVarRegistry::findMatching(const std::string& prefix)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<ConVarBase*> result;

    std::string lowerPrefix = prefix;
    std::transform(lowerPrefix.begin(), lowerPrefix.end(), lowerPrefix.begin(), ::tolower);

    for (auto& [name, cvar] : m_cvars)
    {
        if (cvar->hasFlag(ConVarFlags::HIDDEN))
        {
            continue;
        }

        std::string lowerName = name;
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);

        if (lowerName.find(lowerPrefix) != std::string::npos)
        {
            result.push_back(cvar);
        }
    }

    std::sort(result.begin(), result.end(), [](ConVarBase* a, ConVarBase* b) {
        return a->getName() < b->getName();
    });

    return result;
}

std::vector<ConVarBase*> ConVarRegistry::getAll()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<ConVarBase*> result;
    result.reserve(m_cvars.size());

    for (auto& [name, cvar] : m_cvars)
    {
        result.push_back(cvar);
    }

    std::sort(result.begin(), result.end(), [](ConVarBase* a, ConVarBase* b) {
        return a->getName() < b->getName();
    });

    return result;
}

std::vector<ConVarBase*> ConVarRegistry::getByFlag(uint32_t flag)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<ConVarBase*> result;

    for (auto& [name, cvar] : m_cvars)
    {
        if (cvar->hasFlag(flag))
        {
            result.push_back(cvar);
        }
    }

    return result;
}

std::vector<ConVarBase*> ConVarRegistry::getReplicatedCvars()
{
    return getByFlag(ConVarFlags::REPLICATED);
}

std::vector<ConVarBase*> ConVarRegistry::getCheatCvars()
{
    return getByFlag(ConVarFlags::CHEAT);
}

void ConVarRegistry::enforceCheatRestrictions(bool cheatsEnabled)
{
    if (cheatsEnabled)
    {
        return;  // Cheats enabled, no restrictions
    }

    auto cheatCvars = getCheatCvars();
    for (auto* cvar : cheatCvars)
    {
        cvar->reset();
    }

    LOG_ENGINE_INFO("Cheats disabled - {} cheat cvars reset to defaults", cheatCvars.size());
}

void ConVarRegistry::saveArchiveCvars(const std::string& filepath)
{
    std::ofstream file(filepath);
    if (!file.is_open())
    {
        LOG_ENGINE_ERROR("Could not write config file: {}", filepath);
        return;
    }

    file << "// Auto-generated config file\n";
    file << "// Do not edit while game is running\n\n";

    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& [name, cvar] : m_cvars)
    {
        if (cvar->hasFlag(ConVarFlags::ARCHIVE))
        {
            const std::string& value = cvar->getValueString();
            // Quote strings that contain spaces
            if (value.find(' ') != std::string::npos)
            {
                file << name << " \"" << value << "\"\n";
            }
            else
            {
                file << name << " " << value << "\n";
            }
        }
    }

    LOG_ENGINE_INFO("Config saved to {}", filepath);
}

void ConVarRegistry::loadConfig(const std::string& filepath)
{
    std::ifstream file(filepath);
    if (!file.is_open())
    {
        LOG_ENGINE_TRACE("Config file not found: {}", filepath);
        return;
    }

    LOG_ENGINE_INFO("Loading config: {}", filepath);

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

        // Parse "name value" or "name \"value with spaces\""
        std::string trimmed = line.substr(start);
        size_t spacePos = trimmed.find_first_of(" \t");
        if (spacePos == std::string::npos)
        {
            continue;  // No value provided
        }

        std::string name = trimmed.substr(0, spacePos);
        std::string value = trimmed.substr(spacePos + 1);

        // Trim leading whitespace from value
        size_t valueStart = value.find_first_not_of(" \t");
        if (valueStart != std::string::npos)
        {
            value = value.substr(valueStart);
        }

        // Remove quotes if present
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
        {
            value = value.substr(1, value.size() - 2);
        }

        // Find and set the cvar
        ConVarBase* cvar = find(name);
        if (cvar)
        {
            cvar->setFromString(value);
        }
        else
        {
            LOG_ENGINE_TRACE("{}:{}: Unknown cvar '{}'", filepath, lineNum, name);
        }
    }
}