#pragma once

#include "Animation/AnimationResourceValidation.hpp"
#include "EngineExport.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

// The persisted identity is deliberately small and contains no loader state.
// The path names a cooked CANM package; source glTF is never a runtime fallback.
struct AnimationResourceKey
{
    std::string path;
    uint32_t skin_index = 0;

    bool operator==(const AnimationResourceKey& other) const
    {
        return path == other.path && skin_index == other.skin_index;
    }
};

enum class AnimationResourceState : uint8_t
{
    Unrequested,
    Loading,
    Ready,
    Failed
};

class AnimationResourceSet;
struct AnimationLoadEntry;

class AnimationLoadTicket
{
    friend class AnimationResourceService;
public:
    // An empty ticket is the ECS component's safe initial state until its
    // authored resource key is submitted to the service.
    AnimationLoadTicket() = default;

    bool valid() const { return static_cast<bool>(m_entry); }

private:
    explicit AnimationLoadTicket(std::shared_ptr<AnimationLoadEntry> entry) : m_entry(std::move(entry)) {}
    std::shared_ptr<AnimationLoadEntry> m_entry;
};

// Immutable animation resources are owned here. Entity-local selection, clocks
// and poses live in ECS components and never mutate an entry after publication.
class ENGINE_API AnimationResourceService final
{
public:
    static AnimationResourceService& get();

    AnimationLoadTicket request(const AnimationResourceKey& key);
    std::shared_ptr<const AnimationResourceSet> tryGetReady(const AnimationLoadTicket& ticket) const;
    std::optional<AnimationResourceError> getError(const AnimationLoadTicket& ticket) const;
    AnimationResourceState getState(const AnimationLoadTicket& ticket) const;

    // Removes the canonical cache entry at a safe boundary. Existing tickets are
    // invalidated after any in-flight decode completes; published immutable data
    // remains owned by callers that already retained it.
    bool unload(const AnimationResourceKey& key);

    // Call at the presentation safe boundary. Worker results become visible
    // together here, only after CompiledAnimationSerializer validated all bytes.
    void update();
    void shutdown();

private:
    AnimationResourceService() = default;
    ~AnimationResourceService();
    AnimationResourceService(const AnimationResourceService&) = delete;
    AnimationResourceService& operator=(const AnimationResourceService&) = delete;

    std::string canonicalPackagePath(const AnimationResourceKey& key,
                                     AnimationResourceError& error) const;

    mutable std::mutex m_mutex;
    std::unordered_map<std::string, std::shared_ptr<AnimationLoadEntry>> m_entries;
};
