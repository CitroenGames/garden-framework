#pragma once

#include "Components/IdentityComponent.hpp"
#include "Network/NetworkSerializer.hpp"
#include <cstdint>
#include <memory>
#include <vector>

class world;
namespace Net {
struct SpawnRecipeInfo;
namespace detail {

// EngineCore-private adapter for the client manager's EntityRef, replication,
// and presentation operations. Recipe and replication callbacks currently
// require registry access; raw ECS handles remain inside this bridge.
class ClientNetworkEcsBridge
{
public:
    ClientNetworkEcsBridge();
    ~ClientNetworkEcsBridge();
    void clear();
    void destroyAll(world* w);
    bool contains(uint32_t id) const;
    std::vector<uint32_t> networkIds() const;
    EntityRef entityRef(world* w, uint32_t id) const;
    EntityRef spawnPlayer(world& w, const SpawnPlayerMessage& msg, bool local);
    enum class SpawnResult { Spawned, ConstructionFailed, PendingFailed };
    SpawnResult spawnEntity(world& w, const SpawnEntityMessage& msg,
                     const SpawnRecipeInfo& recipe,
                     const std::vector<ReplicatedExtEntry>& entries,
                     const EntityUpdateData* pending);
    bool localIsRoot(world& w) const;
    bool applyLocalExtensions(world& w, const EntityUpdateData& update);
    enum class UpdateResult { Applied, Missing, Parented, Failed };
    UpdateResult applyUpdate(world& w, const EntityUpdateData& update);
    void seedPresentation(world& w, uint32_t id, const glm::vec3& position, float yaw);
    void clearPresentation(world& w);
    bool erase(world* w, uint32_t id);
    void clearLocal();
    void ensurePresentation(world& w, uint32_t id);
    void setPresentation(world& w, uint32_t id, const glm::vec3& position, float yaw);
private:
    struct Storage;
    std::unique_ptr<Storage> storage;
};
} // namespace detail
} // namespace Net
