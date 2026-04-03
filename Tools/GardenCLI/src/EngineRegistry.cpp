#include "EngineRegistry.hpp"
#include "PathUtils.hpp"
#include "IdGenerator.hpp"
#include "json.hpp"

#include <fstream>
#include <filesystem>
#include <ctime>
#include <cstdio>

namespace fs = std::filesystem;
using json = nlohmann::json;

void EngineRegistry::ensureLoaded() const
{
    if (m_loaded) return;
    m_loaded = true;
    m_entries.clear();

    fs::path config_path = PathUtils::getEnginesJsonPath();
    if (!fs::exists(config_path))
        return;

    std::ifstream file(config_path);
    if (!file.is_open()) return;

    json j;
    try { file >> j; }
    catch (...) { return; }

    if (!j.contains("engines") || !j["engines"].is_object())
        return;

    for (auto& [id, entry] : j["engines"].items())
    {
        EngineEntry e;
        e.id = id;
        e.path = entry.value("path", "");
        e.editor = entry.value("editor", "");
        e.version = entry.value("version", "");
        e.registered_at = entry.value("registered_at", "");
        m_entries.push_back(std::move(e));
    }
}

bool EngineRegistry::save() const
{
    fs::path config_path = PathUtils::getEnginesJsonPath();

    // Create config directory
    std::error_code ec;
    fs::create_directories(config_path.parent_path(), ec);

    json j;
    json engines = json::object();
    for (auto& e : m_entries)
    {
        engines[e.id] = {
            {"path", e.path},
            {"editor", e.editor},
            {"version", e.version},
            {"registered_at", e.registered_at}
        };
    }
    j["engines"] = engines;

    std::ofstream file(config_path);
    if (!file.is_open())
    {
        fprintf(stderr, "Error: Cannot write to '%s'\n", config_path.string().c_str());
        return false;
    }
    file << j.dump(4);
    return true;
}

std::string EngineRegistry::currentTimestamp()
{
    std::time_t now = std::time(nullptr);
    std::tm tm_buf;
#ifdef _WIN32
    localtime_s(&tm_buf, &now);
#else
    localtime_r(&now, &tm_buf);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm_buf);
    return buf;
}

std::string EngineRegistry::registerEngine(const std::string& engine_root_path)
{
    ensureLoaded();

    fs::path root = fs::absolute(engine_root_path);
    if (!fs::is_directory(root))
    {
        fprintf(stderr, "Error: '%s' is not a directory\n", engine_root_path.c_str());
        return "";
    }

    // Check if this path is already registered
    std::string canonical_root = fs::canonical(root).string();
    for (auto& e : m_entries)
    {
        std::error_code ec;
        fs::path existing = fs::canonical(e.path, ec);
        if (!ec && existing.string() == canonical_root)
        {
            printf("Engine already registered with ID: %s\n", e.id.c_str());
            return e.id;
        }
    }

    // Find editor executable
    fs::path editor_path = PathUtils::findEditorPath(root);
    if (editor_path.empty())
    {
        fprintf(stderr, "Warning: Editor executable not found in '%s'\n", root.string().c_str());
        fprintf(stderr, "  The editor will need to be built before opening projects.\n");
        // Still register — user may build later. Store expected path.
        editor_path = root / "bin" / PathUtils::getEditorExeName();
    }

    EngineEntry entry;
    entry.id = IdGenerator::generate();
    entry.path = canonical_root;
    entry.editor = editor_path.string();
    entry.version = "1.0";
    entry.registered_at = currentTimestamp();

    m_entries.push_back(entry);

    if (!save())
        return "";

    return entry.id;
}

bool EngineRegistry::unregisterEngine(const std::string& id)
{
    ensureLoaded();

    auto it = std::remove_if(m_entries.begin(), m_entries.end(),
        [&id](const EngineEntry& e) { return e.id == id; });

    if (it == m_entries.end())
    {
        fprintf(stderr, "Error: No engine with ID '%s'\n", id.c_str());
        return false;
    }

    m_entries.erase(it, m_entries.end());
    return save();
}

std::vector<EngineEntry> EngineRegistry::listEngines() const
{
    ensureLoaded();
    return m_entries;
}

const EngineEntry* EngineRegistry::findEngine(const std::string& id) const
{
    ensureLoaded();
    for (auto& e : m_entries)
        if (e.id == id)
            return &e;
    return nullptr;
}
