#pragma once

#include "Audio/AudioSystem.hpp"
#include "Reflection/Reflector.hpp"

inline const char* audio_group_names[] = { "Master", "SFX", "Music", "Voice", "UI" };

struct AudioSourceComponent
{
    // Authoring (serialized): clip asset + playback tuning. The resolved
    // clip id, active SoundHandle and started flag below are runtime-only
    // and intentionally not reflected.
    std::string clip_path;
    AudioClipId clip = INVALID_AUDIO_CLIP;
    float volume = 1.0f;
    float pitch = 1.0f;
    float min_distance = 1.0f;
    float max_distance = 50.0f;
    bool spatial = true;
    bool loop = false;
    bool auto_play = false;
    bool enabled = true;
    AudioGroup group = AudioGroup::SFX;

    // Runtime state
    SoundHandle active_handle = INVALID_SOUND;
    bool started = false;
    AudioClipId active_clip = INVALID_AUDIO_CLIP;
    std::string resolved_clip_path;
    bool active_spatial = true;
    bool active_loop = false;
    float active_min_distance = 1.0f;
    float active_max_distance = 50.0f;
    uint64_t audio_lifecycle_generation = 0;

    static void reflect(Reflector<AudioSourceComponent>& r)
    {
        r.display("Audio Source").category("Audio");
        r.field<&AudioSourceComponent::clip_path>("clip_path")
            .tooltip("Audio clip asset path").assetPath().category("Clip");
        r.field<&AudioSourceComponent::volume>("volume")
            .tooltip("Playback volume").drag(0.01f).range(0.0f, 4.0f).category("Playback");
        r.field<&AudioSourceComponent::pitch>("pitch")
            .tooltip("Playback pitch").drag(0.01f).range(0.05f, 4.0f).category("Playback");
        r.field<&AudioSourceComponent::min_distance>("min_distance")
            .tooltip("Distance at which attenuation starts").drag(0.1f).range(0.0f, 1000.0f).category("Spatial");
        r.field<&AudioSourceComponent::max_distance>("max_distance")
            .tooltip("Distance at which the sound is inaudible").drag(0.1f).range(0.0f, 10000.0f).category("Spatial");
        r.field<&AudioSourceComponent::spatial>("spatial")
            .tooltip("Use 3D spatial attenuation").category("Spatial");
        r.field<&AudioSourceComponent::loop>("loop")
            .tooltip("Loop playback").category("Playback");
        r.field<&AudioSourceComponent::auto_play>("auto_play")
            .tooltip("Start playing on spawn").category("Playback");
        r.field<&AudioSourceComponent::enabled>("enabled")
            .tooltip("Enable this source").category("Playback");
        r.field<&AudioSourceComponent::group>("group")
            .tooltip("Audio mixer group")
            .enumValues(audio_group_names, (int)AudioGroup::COUNT)
            .category("Clip");
    }
};
