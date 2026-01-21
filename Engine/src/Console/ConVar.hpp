#pragma once

#include <string>
#include <functional>
#include <variant>
#include <vector>
#include <unordered_map>
#include <optional>
#include <mutex>

// ConVar flags
namespace ConVarFlags
{
    constexpr uint32_t NONE         = 0;
    constexpr uint32_t CHEAT        = 1 << 0;   // Requires sv_cheats 1
    constexpr uint32_t REPLICATED   = 1 << 1;   // Server broadcasts to clients
    constexpr uint32_t ARCHIVE      = 1 << 2;   // Saved to config file
    constexpr uint32_t NOTIFY       = 1 << 3;   // Notify players on change
    constexpr uint32_t USERINFO     = 1 << 4;   // Sent to server on connect
    constexpr uint32_t SERVER_ONLY  = 1 << 5;   // Only settable on server
    constexpr uint32_t CLIENT_ONLY  = 1 << 6;   // Only settable on client
    constexpr uint32_t HIDDEN       = 1 << 7;   // Hidden from find/autocomplete
}

// ConVar value types
using ConVarValue = std::variant<int, float, bool, std::string>;

// Forward declaration
class ConVarBase;

// Callback signature for value changes
using ConVarCallback = std::function<void(ConVarBase* cvar, const ConVarValue& oldValue, const ConVarValue& newValue)>;

// Base class for all ConVars
class ConVarBase
{
protected:
    std::string m_name;
    std::string m_description;
    uint32_t m_flags;
    ConVarValue m_defaultValue;
    ConVarValue m_currentValue;
    std::vector<ConVarCallback> m_callbacks;

    // Bounds for numeric types
    std::optional<float> m_minValue;
    std::optional<float> m_maxValue;

public:
    ConVarBase(const std::string& name, int defaultValue, uint32_t flags, const std::string& description);
    ConVarBase(const std::string& name, float defaultValue, uint32_t flags, const std::string& description);
    ConVarBase(const std::string& name, bool defaultValue, uint32_t flags, const std::string& description);
    ConVarBase(const std::string& name, const char* defaultValue, uint32_t flags, const std::string& description);
    ConVarBase(const std::string& name, const std::string& defaultValue, uint32_t flags, const std::string& description);
    virtual ~ConVarBase() = default;

    // Getters
    const std::string& getName() const { return m_name; }
    const std::string& getDescription() const { return m_description; }
    uint32_t getFlags() const { return m_flags; }
    bool hasFlag(uint32_t flag) const { return (m_flags & flag) != 0; }

    // Value access
    int getInt() const;
    float getFloat() const;
    bool getBool() const;
    const std::string& getString() const;
    const ConVarValue& getValue() const { return m_currentValue; }
    const ConVarValue& getDefaultValue() const { return m_defaultValue; }

    // Value setting (with validation and callbacks)
    bool setInt(int value);
    bool setFloat(float value);
    bool setBool(bool value);
    bool setString(const std::string& value);
    bool setFromString(const std::string& valueStr);

    // Reset to default
    void reset();

    // Bounds
    void setBounds(float min, float max);
    bool hasBounds() const { return m_minValue.has_value() && m_maxValue.has_value(); }
    float getMinBound() const { return m_minValue.value_or(0.0f); }
    float getMaxBound() const { return m_maxValue.value_or(0.0f); }

    // Callbacks
    void addChangeCallback(ConVarCallback callback);

    // String representation
    std::string getValueString() const;
    std::string getDefaultValueString() const;

protected:
    void notifyCallbacks(const ConVarValue& oldValue);
    bool validateAndClamp(ConVarValue& value);
    bool validateCheatRestriction() const;
};

// ConVar Registry (Singleton)
class ConVarRegistry
{
public:
    static ConVarRegistry& get();

    // Registration
    void registerConVar(ConVarBase* cvar);
    void unregisterConVar(const std::string& name);

    // Lookup
    ConVarBase* find(const std::string& name);
    std::vector<ConVarBase*> findMatching(const std::string& prefix);
    std::vector<ConVarBase*> getAll();
    std::vector<ConVarBase*> getByFlag(uint32_t flag);

    // Network replication helpers
    std::vector<ConVarBase*> getReplicatedCvars();
    std::vector<ConVarBase*> getCheatCvars();

    // Cheat enforcement
    void enforceCheatRestrictions(bool cheatsEnabled);

    // Config file
    void saveArchiveCvars(const std::string& filepath);
    void loadConfig(const std::string& filepath);

private:
    ConVarRegistry() = default;
    std::unordered_map<std::string, ConVarBase*> m_cvars;
    mutable std::mutex m_mutex;
};

// Static registration helper
class ConVarRegistrar
{
public:
    ConVarRegistrar(ConVarBase* cvar)
    {
        ConVarRegistry::get().registerConVar(cvar);
    }
};

// Main CONVAR macro - creates a global ConVar with auto-registration
#define CONVAR(name, defaultVal, flags, description) \
    static ConVarBase g_cvar_##name(#name, defaultVal, flags, description); \
    static ConVarRegistrar g_cvar_registrar_##name(&g_cvar_##name)

// CONVAR with bounds (for numeric types)
#define CONVAR_BOUNDED(name, defaultVal, minVal, maxVal, flags, description) \
    static ConVarBase g_cvar_##name(#name, defaultVal, flags, description); \
    static struct ConVarInit_##name { \
        ConVarInit_##name() { \
            g_cvar_##name.setBounds(static_cast<float>(minVal), static_cast<float>(maxVal)); \
            ConVarRegistry::get().registerConVar(&g_cvar_##name); \
        } \
    } g_cvar_init_##name

// Helper to get cvar pointer by name
#define CVAR_PTR(name) ConVarRegistry::get().find(#name)

// Helper macros to get cvar values with type
#define CVAR_INT(name) (CVAR_PTR(name) ? CVAR_PTR(name)->getInt() : 0)
#define CVAR_FLOAT(name) (CVAR_PTR(name) ? CVAR_PTR(name)->getFloat() : 0.0f)
#define CVAR_BOOL(name) (CVAR_PTR(name) ? CVAR_PTR(name)->getBool() : false)
#define CVAR_STRING(name) (CVAR_PTR(name) ? CVAR_PTR(name)->getString() : "")

// Force initialization of default cvars (call at startup)
void InitializeDefaultCVars();
