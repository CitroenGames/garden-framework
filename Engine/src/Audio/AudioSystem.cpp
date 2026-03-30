#define MINIAUDIO_IMPLEMENTATION
#include "../../Thirdparty/miniaudio/miniaudio.h"

#include "AudioSystem.hpp"
#include "Utils/Log.hpp"
#include <unordered_map>
#include <algorithm>

struct AudioSystem::AudioClip
{
    AudioClipId id = INVALID_AUDIO_CLIP;
    std::string path;
    ma_decoder decoder;
    bool loaded = false;
};

struct AudioSystem::ActiveSound
{
    SoundHandle handle = INVALID_SOUND;
    AudioClipId clip_id = INVALID_AUDIO_CLIP;
    ma_sound sound;
    AudioGroup group = AudioGroup::SFX;
    bool spatial = false;
    bool active = false;
};

struct AudioSystem::Impl
{
    ma_engine engine;
    bool engine_initialized = false;

    std::unordered_map<AudioClipId, AudioClip> clips;
    std::vector<ActiveSound> sounds;

    // Keep track of loaded ma_sound objects for resource management
    ma_resource_manager_config resource_config;
};

AudioSystem::AudioSystem()
    : impl(std::make_unique<Impl>())
{
}

AudioSystem::~AudioSystem()
{
    if (initialized)
    {
        shutdown();
    }
}

bool AudioSystem::initialize()
{
    if (initialized) return true;

    ma_engine_config config = ma_engine_config_init();
    config.channels = 2;
    config.sampleRate = 44100;
    config.listenerCount = 1;

    ma_result result = ma_engine_init(&config, &impl->engine);
    if (result != MA_SUCCESS)
    {
        LOG_ENGINE_ERROR("Failed to initialize audio engine: {}", static_cast<int>(result));
        return false;
    }

    impl->engine_initialized = true;
    initialized = true;

    LOG_ENGINE_INFO("Audio system initialized (miniaudio)");
    return true;
}

void AudioSystem::shutdown()
{
    if (!initialized) return;

    // Stop and uninit all active sounds
    for (auto& sound : impl->sounds)
    {
        if (sound.active)
        {
            ma_sound_uninit(&sound.sound);
            sound.active = false;
        }
    }
    impl->sounds.clear();

    // Unload all clips
    impl->clips.clear();

    // Shutdown engine
    if (impl->engine_initialized)
    {
        ma_engine_uninit(&impl->engine);
        impl->engine_initialized = false;
    }

    initialized = false;
    LOG_ENGINE_INFO("Audio system shutdown");
}

AudioClipId AudioSystem::loadClip(const std::string& path)
{
    if (!initialized) return INVALID_AUDIO_CLIP;

    // Check if already loaded
    for (auto& [id, clip] : impl->clips)
    {
        if (clip.path == path && clip.loaded)
        {
            return id;
        }
    }

    AudioClipId id = next_clip_id++;
    AudioClip clip;
    clip.id = id;
    clip.path = path;
    clip.loaded = true; // miniaudio loads on demand via engine
    impl->clips[id] = std::move(clip);

    LOG_ENGINE_TRACE("Audio clip registered: {} (id={})", path, id);
    return id;
}

void AudioSystem::unloadClip(AudioClipId id)
{
    // Stop any sounds using this clip
    for (auto& sound : impl->sounds)
    {
        if (sound.active && sound.clip_id == id)
        {
            ma_sound_uninit(&sound.sound);
            sound.active = false;
        }
    }

    impl->clips.erase(id);
}

bool AudioSystem::isClipLoaded(AudioClipId id) const
{
    auto it = impl->clips.find(id);
    return it != impl->clips.end() && it->second.loaded;
}

SoundHandle AudioSystem::playSound(AudioClipId clip_id, const PlayParams& params)
{
    if (!initialized) return INVALID_SOUND;

    auto it = impl->clips.find(clip_id);
    if (it == impl->clips.end()) return INVALID_SOUND;

    SoundHandle handle = next_sound_id++;

    ActiveSound active;
    active.handle = handle;
    active.clip_id = clip_id;
    active.group = params.group;
    active.spatial = false;

    ma_result result = ma_sound_init_from_file(
        &impl->engine,
        it->second.path.c_str(),
        MA_SOUND_FLAG_DECODE, // Decode immediately for low latency
        nullptr, nullptr,
        &active.sound);

    if (result != MA_SUCCESS)
    {
        LOG_ENGINE_ERROR("Failed to play sound '{}': {}", it->second.path, static_cast<int>(result));
        return INVALID_SOUND;
    }

    // Configure
    float effective_volume = params.volume * group_volumes[static_cast<int>(params.group)]
                           * group_volumes[static_cast<int>(AudioGroup::Master)];
    ma_sound_set_volume(&active.sound, effective_volume);
    ma_sound_set_pitch(&active.sound, params.pitch);
    ma_sound_set_looping(&active.sound, params.loop);

    // Disable spatialization for 2D sounds
    ma_sound_set_spatialization_enabled(&active.sound, MA_FALSE);

    ma_sound_start(&active.sound);
    active.active = true;

    impl->sounds.push_back(std::move(active));
    return handle;
}

SoundHandle AudioSystem::playSound3D(AudioClipId clip_id, const glm::vec3& position, const Play3DParams& params)
{
    if (!initialized) return INVALID_SOUND;

    auto it = impl->clips.find(clip_id);
    if (it == impl->clips.end()) return INVALID_SOUND;

    SoundHandle handle = next_sound_id++;

    ActiveSound active;
    active.handle = handle;
    active.clip_id = clip_id;
    active.group = params.group;
    active.spatial = true;

    ma_result result = ma_sound_init_from_file(
        &impl->engine,
        it->second.path.c_str(),
        MA_SOUND_FLAG_DECODE,
        nullptr, nullptr,
        &active.sound);

    if (result != MA_SUCCESS)
    {
        LOG_ENGINE_ERROR("Failed to play 3D sound '{}': {}", it->second.path, static_cast<int>(result));
        return INVALID_SOUND;
    }

    // Configure
    float effective_volume = params.volume * group_volumes[static_cast<int>(params.group)]
                           * group_volumes[static_cast<int>(AudioGroup::Master)];
    ma_sound_set_volume(&active.sound, effective_volume);
    ma_sound_set_pitch(&active.sound, params.pitch);
    ma_sound_set_looping(&active.sound, params.loop);

    // Spatial settings
    ma_sound_set_spatialization_enabled(&active.sound, MA_TRUE);
    ma_sound_set_position(&active.sound, position.x, position.y, position.z);
    ma_sound_set_min_distance(&active.sound, params.min_distance);
    ma_sound_set_max_distance(&active.sound, params.max_distance);
    ma_sound_set_attenuation_model(&active.sound, ma_attenuation_model_inverse);

    ma_sound_start(&active.sound);
    active.active = true;

    impl->sounds.push_back(std::move(active));
    return handle;
}

void AudioSystem::stopSound(SoundHandle handle)
{
    for (auto& sound : impl->sounds)
    {
        if (sound.handle == handle && sound.active)
        {
            ma_sound_stop(&sound.sound);
            ma_sound_uninit(&sound.sound);
            sound.active = false;
            return;
        }
    }
}

void AudioSystem::stopAllSounds()
{
    for (auto& sound : impl->sounds)
    {
        if (sound.active)
        {
            ma_sound_stop(&sound.sound);
            ma_sound_uninit(&sound.sound);
            sound.active = false;
        }
    }
    impl->sounds.clear();
}

bool AudioSystem::isSoundPlaying(SoundHandle handle) const
{
    for (const auto& sound : impl->sounds)
    {
        if (sound.handle == handle && sound.active)
        {
            return ma_sound_is_playing(&sound.sound);
        }
    }
    return false;
}

void AudioSystem::setListenerPosition(const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up)
{
    if (!initialized) return;

    ma_engine_listener_set_position(&impl->engine, 0, position.x, position.y, position.z);
    ma_engine_listener_set_direction(&impl->engine, 0, forward.x, forward.y, forward.z);
    ma_engine_listener_set_world_up(&impl->engine, 0, up.x, up.y, up.z);
}

void AudioSystem::setMasterVolume(float volume)
{
    group_volumes[static_cast<int>(AudioGroup::Master)] = volume;
    if (initialized)
    {
        ma_engine_set_volume(&impl->engine, volume);
    }
}

void AudioSystem::setGroupVolume(AudioGroup group, float volume)
{
    group_volumes[static_cast<int>(group)] = volume;
}

float AudioSystem::getGroupVolume(AudioGroup group) const
{
    return group_volumes[static_cast<int>(group)];
}

void AudioSystem::update()
{
    if (!initialized) return;

    // Clean up finished (non-looping) sounds
    for (auto it = impl->sounds.begin(); it != impl->sounds.end(); )
    {
        if (it->active && !ma_sound_is_playing(&it->sound) && !ma_sound_is_looping(&it->sound))
        {
            ma_sound_uninit(&it->sound);
            it->active = false;
            it = impl->sounds.erase(it);
        }
        else if (!it->active)
        {
            it = impl->sounds.erase(it);
        }
        else
        {
            ++it;
        }
    }
}
