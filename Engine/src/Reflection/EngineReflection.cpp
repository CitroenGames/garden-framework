#include "EngineReflection.hpp"
#include "ReflectionRegistry.hpp"
#include "Components/Components.hpp"
#include "Components/PrefabInstanceComponent.hpp"
#include "Components/HierarchyComponents.hpp"
#include "Components/IdentityComponent.hpp"
#include "Components/AnimationComponent.hpp"
#include "Components/AudioSourceComponent.hpp"
#include "Components/IKComponent.hpp"
#include "GameFramework/GameFrameworkComponents.hpp"
#include "Gameplay/GameplayComponents.hpp"

void registerEngineReflection(ReflectionRegistry& registry)
{
    registry.reflect<TransformComponent>("TransformComponent");
    registry.reflect<TagComponent>("TagComponent");
    registry.reflect<TerrainComponent>("TerrainComponent");
    registry.reflect<RigidBodyComponent>("RigidBodyComponent");
    registry.reflect<ColliderComponent>("ColliderComponent");
    registry.reflect<TriggerComponent>("TriggerComponent");
    registry.reflect<ConstraintComponent>("ConstraintComponent");
    registry.reflect<CharacterControllerComponent>("CharacterControllerComponent");
    registry.reflect<WaterVolumeComponent>("WaterVolumeComponent");
    registry.reflect<WaterComponent>("WaterComponent");
    registry.reflect<PlayerComponent>("PlayerComponent");
    registry.reflect<CameraSpringComponent>("CameraSpringComponent");
    registry.reflect<FreecamComponent>("FreecamComponent");
    registry.reflect<PlayerStartComponent>("PlayerStartComponent");
    registry.reflect<PlayerRepresentationComponent>("PlayerRepresentationComponent");
    registry.reflect<PointLightComponent>("PointLightComponent");
    registry.reflect<SpotLightComponent>("SpotLightComponent");
    registry.reflect<PrefabInstanceComponent>("PrefabInstanceComponent");
    // MeshComponent is exempt from reflection (plan 2.6 / prefabs_serialization.md
    // §4): it holds a shared_ptr<mesh> GPU resource and is persisted via the
    // level/prefab sidecar path (mesh path + render flags), not reflected JSON.
    // InputComponent is likewise exempt: it is a polymorphic input base class,
    // not an ECS data component (see Components/InputComponent.hpp).
    registry.reflect<AnimationComponent>("AnimationComponent");
    registry.reflect<AudioSourceComponent>("AudioSourceComponent");
    registry.reflect<IKComponent>("IKComponent");
    registry.reflect<GameFramework::GameModeComponent>("GameModeComponent");
    registry.reflect<GameFramework::GameStateComponent>("GameStateComponent");
    registry.reflect<GameFramework::PlayerStateComponent>("PlayerStateComponent");
    registry.reflect<GameFramework::PlayerControllerComponent>("PlayerControllerComponent");
    registry.reflect<IdentityComponent>("IdentityComponent");
    // Hierarchy: ParentComponent is the load-authoritative link;
    // ChildrenComponent (vector<EntityRef>) is reflected/serialized and
    // rebuilt from Parent on load. WorldTransformComponent is derived
    // and intentionally unreflected.
    registry.reflect<TeamComponent>("TeamComponent");
    registry.reflect<CountdownTimerComponent>("CountdownTimerComponent");
    registry.reflect<ResourceWalletComponent>("ResourceWalletComponent");
    registry.reflect<InventoryComponent>("InventoryComponent");
    registry.reflect<HealthComponent>("HealthComponent");
    registry.reflect<ChannelComponent>("ChannelComponent");
    registry.reflect<ParentComponent>("ParentComponent");
    registry.reflect<ChildrenComponent>("ChildrenComponent");
    registry.reflect<HierarchyPolicyComponent>("HierarchyPolicyComponent");
}
