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
    if (!audio.isInitialized()) return;

    auto view = registry.view<AudioSourceComponent, TransformComponent>();
    for (entt::entity e : view)
    {
        auto& src = view.get<AudioSourceComponent>(e);
        const auto world_position = Hierarchy::resolveSimulationWorldPosition(registry, e);
        if (src.clip == INVALID_AUDIO_CLIP && !src.clip_path.empty())
            src.clip = audio.loadClip(src.clip_path);

        if (src.auto_play && !src.started)
        {
            if (src.clip == INVALID_AUDIO_CLIP) continue;
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
                map[e] = h;
            }
        }
        if (src.started && src.active_handle != INVALID_SOUND && src.spatial)
        {
            if (world_position)
                audio.setSoundPosition(src.active_handle, *world_position);
        }
    }
}

void shutdown(entt::registry& /*registry*/)
{
    auto& audio = AudioSystem::get();
    for (auto& [e, h] : tracked()) audio.stopSound(h);
    tracked().clear();
}
}
