#include "Audio/AudioSourceSystem.hpp"
#include "Audio/AudioSystem.hpp"
#include "Components/AudioSourceComponent.hpp"
#include "Components/Components.hpp"
#include "ECS/Hierarchy.hpp"
#include <unordered_map>

namespace
{
// Handles started by this system, for sweeping entities that died
// (their component is gone by the time we would notice).
std::unordered_map<entt::entity, SoundHandle>& tracked()
{
    static std::unordered_map<entt::entity, SoundHandle> map;
    return map;
}

void clearRuntime(AudioSourceComponent& source)
{
    source.active_handle = INVALID_SOUND;
    source.started = false;
    source.active_clip = INVALID_AUDIO_CLIP;
    source.resolved_clip_path.clear();
    source.audio_lifecycle_generation = 0;
}

void stopSource(AudioSystem& audio, std::unordered_map<entt::entity, SoundHandle>& map,
                entt::entity entity, AudioSourceComponent& source)
{
    const auto tracked_source = map.find(entity);
    if (tracked_source != map.end())
    {
        audio.stopSound(tracked_source->second);
        map.erase(tracked_source);
    }
    else if (source.active_handle != INVALID_SOUND)
    {
        audio.stopSound(source.active_handle);
    }
    clearRuntime(source);
}

bool requiresRestart(const AudioSourceComponent& source)
{
    return source.active_handle != INVALID_SOUND &&
        (source.active_clip != source.clip ||
         source.resolved_clip_path != source.clip_path ||
         source.active_spatial != source.spatial ||
         source.active_loop != source.loop ||
         source.active_min_distance != source.min_distance ||
         source.active_max_distance != source.max_distance);
}
}

namespace AudioSourceSystem
{
void update(entt::registry& registry, float /*dt*/)
{
    auto& audio = AudioSystem::get();
    auto& map = tracked();

    // Sweep dead/removed sources: stop within one frame of death.
    for (auto it = map.begin(); it != map.end(); )
    {
        entt::entity e = it->first;
        if (!registry.valid(e) || !registry.all_of<AudioSourceComponent>(e))
        {
            audio.stopSound(it->second);
            it = map.erase(it);
        }
        else ++it;
    }

    // A mixer restart invalidates every miniaudio handle and every registered
    // clip. Do this before the initialized check so sources loaded before a
    // device becomes available start cleanly on the following frame.
    auto source_view = registry.view<AudioSourceComponent>();
    for (entt::entity e : source_view)
    {
        auto& src = source_view.get<AudioSourceComponent>(e);
        if (src.audio_lifecycle_generation != 0 &&
            src.audio_lifecycle_generation != audio.lifecycleGeneration())
        {
            stopSource(audio, map, e, src);
            if (!src.clip_path.empty()) src.clip = INVALID_AUDIO_CLIP;
        }
    }
    if (!audio.isInitialized()) return;

    auto view = registry.view<AudioSourceComponent, TransformComponent>();
    for (entt::entity e : view)
    {
        auto& src = view.get<AudioSourceComponent>(e);
        const auto world_position = Hierarchy::resolveSimulationWorldPosition(registry, e);

        // A component can be removed and re-added to the same entity between
        // presentation frames. The old map entry belongs to the removed
        // component and must not leak into the new one.
        const auto tracked_source = map.find(e);
        if (tracked_source != map.end() && tracked_source->second != src.active_handle)
        {
            audio.stopSound(tracked_source->second);
            map.erase(tracked_source);
        }

        if (!src.enabled || !src.auto_play)
        {
            if (src.started || src.active_handle != INVALID_SOUND || map.contains(e))
                stopSource(audio, map, e, src);
            continue;
        }

        if (!src.clip_path.empty() && src.resolved_clip_path != src.clip_path)
        {
            if (src.started) stopSource(audio, map, e, src);
            src.clip = INVALID_AUDIO_CLIP;
        }
        if (src.clip == INVALID_AUDIO_CLIP && !src.clip_path.empty())
        {
            src.clip = audio.loadClip(src.clip_path);
            if (src.clip != INVALID_AUDIO_CLIP)
                src.resolved_clip_path = src.clip_path;
        }

        if (requiresRestart(src))
            stopSource(audio, map, e, src);

        if (src.auto_play && !src.started)
        {
            if (src.clip == INVALID_AUDIO_CLIP || !audio.isClipLoaded(src.clip)) continue;
            SoundHandle h = INVALID_SOUND;
            if (src.spatial)
            {
                if (!world_position) continue;
                Play3DParams p; p.volume = src.volume; p.pitch = src.pitch; p.loop = src.loop;
                p.group = src.group; p.min_distance = src.min_distance; p.max_distance = src.max_distance;
                h = audio.playSound3D(src.clip, *world_position, p);
            }
            else
            {
                PlayParams p; p.volume = src.volume; p.pitch = src.pitch; p.loop = src.loop; p.group = src.group;
                h = audio.playSound(src.clip, p);
            }
            if (h != INVALID_SOUND)
            {
                src.active_handle = h;
                src.started = true;
                src.active_clip = src.clip;
                src.resolved_clip_path = src.clip_path;
                src.active_spatial = src.spatial;
                src.active_loop = src.loop;
                src.active_min_distance = src.min_distance;
                src.active_max_distance = src.max_distance;
                src.audio_lifecycle_generation = audio.lifecycleGeneration();
                map[e] = h;
            }
        }
        if (src.started && src.active_handle != INVALID_SOUND && src.spatial)
        {
            if (world_position)
                audio.setSoundPosition(src.active_handle, *world_position);
        }
        if (src.started && src.active_handle != INVALID_SOUND)
        {
            audio.setSoundVolume(src.active_handle, src.volume);
            audio.setSoundPitch(src.active_handle, src.pitch);
        }
    }
}

void shutdown(entt::registry& registry)
{
    auto& audio = AudioSystem::get();
    auto& map = tracked();
    auto view = registry.view<AudioSourceComponent>();
    for (entt::entity e : view)
        stopSource(audio, map, e, view.get<AudioSourceComponent>(e));

    // A source may belong to a registry already being destroyed and therefore
    // not be visible above. Stop the remaining handles as a final ownership
    // sweep.
    for (auto& [entity, handle] : map) audio.stopSound(handle);
    map.clear();
}
}
