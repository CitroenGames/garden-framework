#include "Network/ClientNetworkEcsBridge.hpp"
#include "Network/NetworkHierarchyValidation.hpp"
#include "Network/ReplicationRegistry.hpp"
#include "Network/SpawnRecipeRegistry.hpp"
#include "Components/Components.hpp"
#include "world.hpp"
#include "Utils/Log.hpp"
#include <entt/entt.hpp>
#include <unordered_map>

namespace Net::detail {
struct ClientNetworkEcsBridge::Storage {
    std::unordered_map<uint32_t, entt::entity> entities;
    entt::entity local = entt::null;
};
ClientNetworkEcsBridge::ClientNetworkEcsBridge() : storage(std::make_unique<Storage>()) {}
ClientNetworkEcsBridge::~ClientNetworkEcsBridge() = default;
void ClientNetworkEcsBridge::clear() { storage->entities.clear(); storage->local = entt::null; }
void ClientNetworkEcsBridge::destroyAll(world* w) {
    if (w) for (auto [id, entity] : storage->entities) {
        (void)id;
        if (w->registry.valid(entity)) w->registry.destroy(entity);
    }
    clear();
}
bool ClientNetworkEcsBridge::contains(uint32_t id) const { return storage->entities.contains(id); }
std::vector<uint32_t> ClientNetworkEcsBridge::networkIds() const {
    std::vector<uint32_t> ids; ids.reserve(storage->entities.size());
    for (auto [id, entity] : storage->entities) { (void)entity; ids.push_back(id); }
    return ids;
}
EntityRef ClientNetworkEcsBridge::entityRef(world* w, uint32_t id) const {
    auto it = storage->entities.find(id);
    return !w || it == storage->entities.end() ? EntityRef{} : entityRefOf(w->registry, it->second);
}
EntityRef ClientNetworkEcsBridge::spawnPlayer(world& w, const SpawnPlayerMessage& msg, bool local) {
    // Create entity
    entt::entity entity = w.registry.create();

    // Add NetworkedEntity component
    NetworkedEntity networked;
    networked.network_id = msg.entity_id;
    networked.owner_client_id = msg.client_id;
    networked.is_player = true;
    w.registry.emplace<NetworkedEntity>(entity, networked);

    // Add TransformComponent
    TransformComponent transform;
    transform.position = msg.position;
    // D4 point 5: wire yaw is radians; ECS rotation is degrees.
    transform.rotation.y = glm::degrees(msg.camera_yaw);
    w.registry.emplace<TransformComponent>(entity, transform);

    RigidBodyComponent rigidbody;
    rigidbody.mass = 80.0f;
    rigidbody.apply_gravity = false;
    w.registry.emplace<RigidBodyComponent>(entity, rigidbody);

    PlayerComponent player;
    player.speed = 10.0f;
    player.jump_force = 5.0f;
    player.input_enabled = true;
    w.registry.emplace<PlayerComponent>(entity, player);

    CharacterControllerComponent controller;
    controller.move_speed = player.speed;
    controller.jump_velocity = player.jump_force;
    controller.capsule_half_height = player.capsule_half_height;
    controller.capsule_radius = player.capsule_radius;
    controller.mass = rigidbody.mass;
    controller.input_enabled = player.input_enabled;
    w.registry.emplace<CharacterControllerComponent>(entity, controller);

    // Assign a Garden identity before publishing the network entity.
    const EntityRef entity_ref = ensureEntityRef(w.registry, entity);
    if (!w.isEntityValid(w.scopedEntity(entity_ref)))
        LOG_ENGINE_ERROR("Could not register scoped identity for network player {}", msg.entity_id);
    storage->entities[msg.entity_id] = entity;
    if (local) storage->local = entity;
    return entity_ref;
}
static bool applyUpdateToEntity(world* game_world, entt::entity entity, const EntityUpdateData& update)
{
    if (game_world == nullptr || !game_world->registry.valid(entity)) {
        return false;
    }

    if (update.hasTransform()) {
        if (game_world->registry.all_of<TransformComponent>(entity)) {
            game_world->registry.get<TransformComponent>(entity).position = update.position;
        } else {
            TransformComponent transform;
            transform.position = update.position;
            game_world->registry.emplace<TransformComponent>(entity, transform);
        }
    }
    if (update.hasRotation() && game_world->registry.all_of<TransformComponent>(entity)) {
        game_world->registry.get<TransformComponent>(entity).rotation.y = glm::degrees(update.rotation_y);
    }
    if (update.hasVelocity()) {
        if (game_world->registry.all_of<RigidBodyComponent>(entity)) {
            game_world->registry.get<RigidBodyComponent>(entity).velocity = update.velocity;
        } else {
            RigidBodyComponent body;
            body.velocity = update.velocity;
            game_world->registry.emplace<RigidBodyComponent>(entity, body);
        }
    }
    if (update.hasGrounded()) {
        if (game_world->registry.all_of<PlayerComponent>(entity)) {
            auto& player = game_world->registry.get<PlayerComponent>(entity);
            player.grounded = update.grounded != 0;
            player.ground_normal = update.ground_normal;
        }
        if (game_world->registry.all_of<CharacterControllerComponent>(entity)) {
            auto& controller = game_world->registry.get<CharacterControllerComponent>(entity);
            controller.grounded = update.grounded != 0;
            controller.ground_normal = update.ground_normal;
        }
    }

    if (!ReplicationRegistry::get().applyEcsEntries(game_world->registry, entity, update.ext_entries)) {
        LOG_ENGINE_ERROR("Validated ECS extension failed to apply for network id {}", update.entity_id);
        return false;
    }
    return true;
}

ClientNetworkEcsBridge::SpawnResult ClientNetworkEcsBridge::spawnEntity(world& w, const SpawnEntityMessage& msg,
    const SpawnRecipeInfo& recipe, const std::vector<ReplicatedExtEntry>& entries,
    const EntityUpdateData* pending) {
    const entt::entity entity = w.registry.create();
    NetworkedEntity networked;
    networked.network_id = msg.network_id;
    networked.owner_client_id = msg.owner_client_id;
    networked.is_player = false;
    w.registry.emplace<NetworkedEntity>(entity, networked);
    TransformComponent transform;
    transform.position = msg.position;
    transform.rotation.y = glm::degrees(msg.rotation_y);
    w.registry.emplace<TransformComponent>(entity, transform);
    if (!recipe.construct(w.registry, entity, msg) ||
        !ReplicationRegistry::get().applyEcsEntries(w.registry, entity, entries)) {
        w.registry.destroy(entity);
        return SpawnResult::ConstructionFailed;
    }
    if (pending && !applyUpdateToEntity(&w, entity, *pending)) {
        w.registry.destroy(entity);
        return SpawnResult::PendingFailed;
    }
    ensureEntityRef(w.registry, entity);
    storage->entities.emplace(msg.network_id, entity);
    return SpawnResult::Spawned;
}
bool ClientNetworkEcsBridge::localIsRoot(world& w) const {
    return isNetworkHierarchyRoot(w.registry, storage->local);
}
bool ClientNetworkEcsBridge::applyLocalExtensions(world& w, const EntityUpdateData& update) {
    return ReplicationRegistry::get().applyEcsEntries(w.registry, storage->local, update.ext_entries);
}
ClientNetworkEcsBridge::UpdateResult ClientNetworkEcsBridge::applyUpdate(world& w, const EntityUpdateData& update) {
    auto it = storage->entities.find(update.entity_id);
    if (it == storage->entities.end() || !w.registry.valid(it->second)) return UpdateResult::Missing;
    if (!isNetworkHierarchyRoot(w.registry, it->second)) return UpdateResult::Parented;
    return applyUpdateToEntity(&w, it->second, update) ? UpdateResult::Applied : UpdateResult::Failed;
}
void ClientNetworkEcsBridge::seedPresentation(world& w, uint32_t id, const glm::vec3& position, float yaw) {
    auto it = storage->entities.find(id);
    if (it == storage->entities.end() || !w.registry.valid(it->second)) return;
    PresentationTransformComponent presentation;
    if (const auto* transform = w.registry.try_get<TransformComponent>(it->second)) presentation.rotation = transform->rotation;
    presentation.position = position;
    presentation.rotation.y = glm::degrees(yaw);
    w.registry.emplace_or_replace<PresentationTransformComponent>(it->second, presentation);
}
void ClientNetworkEcsBridge::clearPresentation(world& w) {
    for (auto [id, entity] : storage->entities) { (void)id;
        if (w.registry.valid(entity)) w.registry.remove<PresentationTransformComponent>(entity);
    }
}
bool ClientNetworkEcsBridge::erase(world* w, uint32_t id) {
    auto it = storage->entities.find(id);
    if (it == storage->entities.end()) return false;
    if (w && w->registry.valid(it->second)) {
        w->getPhysicsSystem().removeBody(it->second);
        w->registry.destroy(it->second);
    }
    storage->entities.erase(it);
    return true;
}
void ClientNetworkEcsBridge::clearLocal() { storage->local = entt::null; }
void ClientNetworkEcsBridge::ensurePresentation(world& w, uint32_t id) {
    auto it = storage->entities.find(id);
    if (it == storage->entities.end() || !w.registry.valid(it->second) ||
        !w.registry.all_of<TransformComponent>(it->second) ||
        w.registry.all_of<PresentationTransformComponent>(it->second)) return;
    const auto& transform = w.registry.get<TransformComponent>(it->second);
    PresentationTransformComponent initial;
    initial.position = transform.position; initial.rotation = transform.rotation;
    w.registry.emplace<PresentationTransformComponent>(it->second, initial);
}
void ClientNetworkEcsBridge::setPresentation(world& w, uint32_t id, const glm::vec3& position, float yaw) {
    auto it = storage->entities.find(id);
    if (it == storage->entities.end() || !w.registry.valid(it->second) ||
        !w.registry.all_of<TransformComponent>(it->second)) return;
    if (auto* presentation = w.registry.try_get<PresentationTransformComponent>(it->second)) {
        presentation->position = position; presentation->rotation.y = glm::degrees(yaw);
    }
}
} // namespace Net::detail
