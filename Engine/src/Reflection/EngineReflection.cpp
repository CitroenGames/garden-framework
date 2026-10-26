#include "EngineReflection.hpp"
#include "ReflectionRegistry.hpp"
#include "Components/Components.hpp"
#include "Components/PrefabInstanceComponent.hpp"
#include "Components/HierarchyComponents.hpp"
#include "Components/IdentityComponent.hpp"

void registerEngineReflection(ReflectionRegistry& registry)
{
    registry.reflect<TransformComponent>("TransformComponent");
    registry.reflect<TagComponent>("TagComponent");
    registry.reflect<TerrainComponent>("TerrainComponent");
    registry.reflect<RigidBodyComponent>("RigidBodyComponent");
    registry.reflect<ColliderComponent>("ColliderComponent");
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
    registry.reflect<IdentityComponent>("IdentityComponent");
    // Hierarchy: ParentComponent is the load-authoritative link;
    // ChildrenComponent (vector<EntityRef>) is reflected/serialized and
    // rebuilt from Parent on load. WorldTransformComponent is derived
    // and intentionally unreflected.
    registry.reflect<ParentComponent>("ParentComponent");
    registry.reflect<ChildrenComponent>("ChildrenComponent");
    registry.reflect<HierarchyPolicyComponent>("HierarchyPolicyComponent");
}
