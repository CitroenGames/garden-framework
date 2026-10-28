#include "Audio/SoundEventTable.hpp"

#include "json.hpp"

#include <cmath>
#include <limits>
#include <unordered_set>

using json = nlohmann::json;

namespace
{
constexpr const char* SchemaName = "garden.sound-events";

void setError(std::string* error, const std::string& message)
{
    if (error)
        *error = message;
}

const char* groupName(AudioGroup group)
{
    switch (group)
    {
    case AudioGroup::Master: return "master";
    case AudioGroup::SFX: return "sfx";
    case AudioGroup::Music: return "music";
    case AudioGroup::Voice: return "voice";
    case AudioGroup::UI: return "ui";
    default: return nullptr;
    }
}

bool parseGroup(const std::string& name, AudioGroup& group)
{
    if (name == "master") { group = AudioGroup::Master; return true; }
    if (name == "sfx") { group = AudioGroup::SFX; return true; }
    if (name == "music") { group = AudioGroup::Music; return true; }
    if (name == "voice") { group = AudioGroup::Voice; return true; }
    if (name == "ui") { group = AudioGroup::UI; return true; }
    return false;
}

bool isFinite(float value)
{
    return std::isfinite(value);
}

bool readFloat(const json& object, const char* key, float& output)
{
    if (!object.contains(key) || !object.at(key).is_number())
        return false;
    output = object.at(key).get<float>();
    return isFinite(output);
}

bool readInt(const json& object, const char* key, int& output)
{
    if (!object.contains(key) || !object.at(key).is_number_integer())
        return false;
    const int64_t value = object.at(key).get<int64_t>();
    if (value < static_cast<int64_t>(std::numeric_limits<int>::min()) ||
        value > static_cast<int64_t>(std::numeric_limits<int>::max()))
        return false;
    output = static_cast<int>(value);
    return true;
}

bool decodeDefinition(const json& value, SoundEventDef& definition, std::string* error)
{
    if (!value.is_object())
    {
        setError(error, "event definition must be an object");
        return false;
    }
    if (!value.contains("id") || !value.at("id").is_string())
    {
        setError(error, "event definition has no string id");
        return false;
    }
    definition.name = value.at("id").get<std::string>();
    if (!value.contains("clips") || !value.at("clips").is_array())
    {
        setError(error, "event '" + definition.name + "' has no clip list");
        return false;
    }
    definition.clip_paths.clear();
    for (const json& clip : value.at("clips"))
    {
        if (!clip.is_string())
        {
            setError(error, "event '" + definition.name + "' has a non-string clip path");
            return false;
        }
        definition.clip_paths.push_back(clip.get<std::string>());
    }
    if (!readFloat(value, "volume", definition.volume) ||
        !readFloat(value, "pitch_min", definition.pitch_min) ||
        !readFloat(value, "pitch_max", definition.pitch_max) ||
        !readFloat(value, "min_distance", definition.min_distance) ||
        !readFloat(value, "max_distance", definition.max_distance) ||
        !readInt(value, "priority", definition.priority) ||
        !readInt(value, "max_voices", definition.max_voices) ||
        !value.contains("loop") || !value.at("loop").is_boolean() ||
        !value.contains("group") || !value.at("group").is_string())
    {
        setError(error, "event '" + definition.name + "' has a missing or invalid field");
        return false;
    }
    definition.loop = value.at("loop").get<bool>();
    if (!parseGroup(value.at("group").get<std::string>(), definition.group))
    {
        setError(error, "event '" + definition.name + "' has an unknown group");
        return false;
    }
    return true;
}
} // namespace

bool SoundEventTable::create(uint32_t version, std::vector<SoundEventDef> definitions,
                             SoundEventTable& output, std::string* error)
{
    SoundEventTable candidate;
    candidate.m_version = version;
    candidate.m_definitions = std::move(definitions);
    if (!candidate.validate(error))
        return false;
    output = std::move(candidate);
    return true;
}

bool SoundEventTable::deserialize(const std::string& document, SoundEventTable& output,
                                  std::string* error)
{
    const json root = json::parse(document, nullptr, false);
    if (root.is_discarded() || !root.is_object())
    {
        setError(error, "sound-event document is not a JSON object");
        return false;
    }
    if (!root.contains("schema") || !root.at("schema").is_string() ||
        root.at("schema").get<std::string>() != SchemaName ||
        !root.contains("version") || !root.at("version").is_number_unsigned() ||
        !root.contains("events") || !root.at("events").is_array())
    {
        setError(error, "sound-event document has an invalid schema header");
        return false;
    }

    const uint64_t version = root.at("version").get<uint64_t>();
    if (version > std::numeric_limits<uint32_t>::max())
    {
        setError(error, "sound-event document version is out of range");
        return false;
    }

    std::vector<SoundEventDef> definitions;
    definitions.reserve(root.at("events").size());
    for (const json& event : root.at("events"))
    {
        SoundEventDef definition;
        if (!decodeDefinition(event, definition, error))
            return false;
        definitions.push_back(std::move(definition));
    }
    return create(static_cast<uint32_t>(version), std::move(definitions), output, error);
}

bool SoundEventTable::serialize(std::string& document, std::string* error) const
{
    if (!validate(error))
        return false;

    json root;
    root["schema"] = SchemaName;
    root["version"] = m_version;
    root["events"] = json::array();
    for (const SoundEventDef& definition : m_definitions)
    {
        const char* group = groupName(definition.group);
        if (!group)
        {
            setError(error, "event '" + definition.name + "' has an invalid group");
            return false;
        }
        root["events"].push_back({
            {"id", definition.name},
            {"clips", definition.clip_paths},
            {"volume", definition.volume},
            {"pitch_min", definition.pitch_min},
            {"pitch_max", definition.pitch_max},
            {"group", group},
            {"min_distance", definition.min_distance},
            {"max_distance", definition.max_distance},
            {"priority", definition.priority},
            {"max_voices", definition.max_voices},
            {"loop", definition.loop},
        });
    }
    document = root.dump();
    return true;
}

bool SoundEventTable::validate(std::string* error) const
{
    if (m_version != CurrentVersion)
    {
        setError(error, "unsupported sound-event table version " + std::to_string(m_version));
        return false;
    }

    std::unordered_set<std::string> ids;
    for (const SoundEventDef& definition : m_definitions)
    {
        if (definition.name.empty())
        {
            setError(error, "sound event id is empty");
            return false;
        }
        if (!ids.emplace(definition.name).second)
        {
            setError(error, "duplicate sound event id '" + definition.name + "'");
            return false;
        }
        if (definition.clip_paths.empty())
        {
            setError(error, "event '" + definition.name + "' has no clips");
            return false;
        }
        for (const std::string& clip : definition.clip_paths)
        {
            if (clip.empty())
            {
                setError(error, "event '" + definition.name + "' has an empty clip path");
                return false;
            }
        }
        if (!isFinite(definition.volume) || definition.volume < 0.0f ||
            !isFinite(definition.pitch_min) || !isFinite(definition.pitch_max) ||
            definition.pitch_min <= 0.0f || definition.pitch_max < definition.pitch_min ||
            !isFinite(definition.min_distance) || !isFinite(definition.max_distance) ||
            definition.min_distance < 0.0f || definition.max_distance < definition.min_distance ||
            definition.priority < 0 || definition.priority > 100 || definition.max_voices < 0 ||
            !groupName(definition.group))
        {
            setError(error, "event '" + definition.name + "' has invalid playback bounds");
            return false;
        }
    }
    return true;
}
