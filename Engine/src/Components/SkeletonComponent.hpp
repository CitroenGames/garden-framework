#pragma once

#include "Animation/AnimationResourceService.hpp"
#include "Reflection/Reflector.hpp"

#include <string>

// Reflected authoring binding. The resolved immutable set and all load state
// belong to AnimResourceBindingComponent and are never serialized.
struct SkeletonComponent
{
    // Kept scalar because the reflection format currently has no nested-object
    // property support. Together these form AnimationResourceKey.
    std::string animation_resource_path;
    uint32_t animation_skin_index = 0;
    std::string default_clip;
    float playback_speed = 1.0f;
    bool loop = true;
    bool auto_play = false;

    AnimationResourceKey resourceKey() const
    {
        return {animation_resource_path, animation_skin_index};
    }

    static void reflect(Reflector<SkeletonComponent>& r)
    {
        r.display("Skeleton").category("Animation");
        // The current reflector has scalar fields only. These two fields are
        // the stable serialized representation of AnimationResourceKey.
        r.field<&SkeletonComponent::animation_resource_path>("animation_resource_path")
            .tooltip("Cooked CANM resource path").assetPath().category("Assets");
        r.field<&SkeletonComponent::animation_skin_index>("animation_skin_index")
            .tooltip("Skin table index in the cooked CANM resource").category("Assets");
        r.field<&SkeletonComponent::default_clip>("default_clip")
            .tooltip("Clip played on spawn when auto_play is set").category("Clips");
        r.field<&SkeletonComponent::playback_speed>("playback_speed")
            .tooltip("Playback speed multiplier").drag(0.01f).range(0.0f, 10.0f).category("Playback");
        r.field<&SkeletonComponent::loop>("loop")
            .tooltip("Loop the default clip").category("Playback");
        r.field<&SkeletonComponent::auto_play>("auto_play")
            .tooltip("Play default_clip automatically").category("Playback");
    }
};
