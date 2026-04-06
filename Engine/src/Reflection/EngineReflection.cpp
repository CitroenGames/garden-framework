#include "EngineReflection.hpp"
#include "ReflectionRegistry.hpp"
#include "Components/Components.hpp"
#include "Components/PrefabInstanceComponent.hpp"

void registerEngineReflection(ReflectionRegistry& registry)
{
    registry.reflect<TransformComponent>("TransformComponent");
    registry.reflect<TagComponent>("TagComponent");
    registry.reflect<RigidBodyComponent>("RigidBodyComponent");
    registry.reflect<PlayerComponent>("PlayerComponent");
    registry.reflect<FreecamComponent>("FreecamComponent");
    registry.reflect<PlayerRepresentationComponent>("PlayerRepresentationComponent");
    registry.reflect<PointLightComponent>("PointLightComponent");
    registry.reflect<SpotLightComponent>("SpotLightComponent");
    registry.reflect<PrefabInstanceComponent>("PrefabInstanceComponent");
}
