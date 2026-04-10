#pragma once

#include <string>
#include <vector>

struct EngineEntry
{
    std::string id;
    std::string path;           // Engine root directory
    std::string editor;         // Absolute path to editor executable
    std::string version;
    std::string registered_at;  // ISO 8601 timestamp
    bool path_exists = true;    // Runtime-only, not persisted to JSON
};

class EngineRegistry
{
public:
    // Register an engine installation. Auto-detects editor location.
    // Returns the generated engine ID, or empty string on failure.
    std::string registerEngine(const std::string& engine_root_path);

    // Remove an engine by ID.
    bool unregisterEngine(const std::string& id);

    // Get all registered engines.
    std::vector<EngineEntry> listEngines() const;

    // Find a specific engine by ID. Returns nullptr if not found.
    // The returned pointer is only valid until the next mutating call.
    const EngineEntry* findEngine(const std::string& id) const;

    // Remove engines whose directories no longer exist. Returns count removed.
    int removeStaleEngines();

private:
    mutable std::vector<EngineEntry> m_entries;
    mutable bool m_loaded = false;

    void ensureLoaded() const;
    bool save() const;
    static std::string currentTimestamp();
};
