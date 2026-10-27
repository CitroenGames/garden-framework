#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include "AudioSystem.hpp"
#include "Utils/Log.hpp"
#include <unordered_map>
#include <algorithm>
#include <cmath>

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
    // A sound is attached to miniaudio's node graph during initialization. Keep
    // it separately allocated so moving this record as the voice vector grows
    // cannot invalidate the address retained by that graph.
    std::unique_ptr<ma_sound> sound;
    AudioGroup group = AudioGroup::SFX;
    bool spatial = false;
    bool active = false;
    float base_volume = 1.0f;
    float pitch = 1.0f;
    glm::vec3 position{0.0f};
    bool paused = false;
};

struct AudioSystem::Impl
{
    ma_engine engine;
    bool engine_initialized = false;
    std::unique_ptr<ma_log> no_device_log;

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

bool AudioSystem::initialize(AudioBackendMode mode)
{
    if (initialized) return true;

    ma_engine_config config = ma_engine_config_init();
    config.channels = 2;
    config.sampleRate = 44100;
    config.listenerCount = 1;
    config.noDevice = mode == AudioBackendMode::NoDevice ? MA_TRUE : MA_FALSE;

    if (mode == AudioBackendMode::NoDevice)
    {
        impl->no_device_log = std::make_unique<ma_log>();
        ma_result log_result = ma_log_init(nullptr, impl->no_device_log.get());
        if (log_result != MA_SUCCESS)
        {
            impl->no_device_log.reset();
            LOG_ENGINE_ERROR("Failed to initialize no-device audio log: {}", static_cast<int>(log_result));
            return false;
        }

        config.pLog = impl->no_device_log.get();
    }

    ma_result result = ma_engine_init(&config, &impl->engine);
    if (result != MA_SUCCESS)
    {
        if (impl->no_device_log)
        {
            ma_log_uninit(impl->no_device_log.get());
            impl->no_device_log.reset();
        }
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
            ma_sound_uninit(sound.sound.get());
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

    if (impl->no_device_log)
    {
        ma_log_uninit(impl->no_device_log.get());
        impl->no_device_log.reset();
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
            ma_sound_uninit(sound.sound.get());
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
    if (!std::isfinite(params.volume) || !std::isfinite(params.pitch)) return INVALID_SOUND;

    auto it = impl->clips.find(clip_id);
    if (it == impl->clips.end()) return INVALID_SOUND;

    SoundHandle handle = next_sound_id++;

    impl->sounds.emplace_back();
    ActiveSound& active = impl->sounds.back();
    active.handle = handle;
    active.clip_id = clip_id;
    active.group = params.group;
    active.spatial = false;
    active.base_volume = params.volume;
    active.pitch = params.pitch;
    active.sound = std::make_unique<ma_sound>();

    ma_result result = ma_sound_init_from_file(
        &impl->engine,
        it->second.path.c_str(),
        MA_SOUND_FLAG_DECODE, // Decode immediately for low latency
        nullptr, nullptr,
        active.sound.get());

    if (result != MA_SUCCESS)
    {
        LOG_ENGINE_ERROR("Failed to play sound '{}': {}", it->second.path, static_cast<int>(result));
        impl->sounds.pop_back();
        return INVALID_SOUND;
    }

    // Configure
    float effective_volume = params.volume * group_volumes[static_cast<int>(params.group)]
                           * group_volumes[static_cast<int>(AudioGroup::Master)];
    ma_sound_set_volume(active.sound.get(), effective_volume);
    ma_sound_set_pitch(active.sound.get(), params.pitch);
    ma_sound_set_looping(active.sound.get(), params.loop);

    // Disable spatialization for 2D sounds
    ma_sound_set_spatialization_enabled(active.sound.get(), MA_FALSE);

    ma_sound_start(active.sound.get());
    active.active = true;

    return handle;
}

SoundHandle AudioSystem::playSound3D(AudioClipId clip_id, const glm::vec3& position, const Play3DParams& params)
{
    if (!initialized) return INVALID_SOUND;
    if (!std::isfinite(params.volume) || !std::isfinite(params.pitch) || !std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z)) return INVALID_SOUND;

    auto it = impl->clips.find(clip_id);
    if (it == impl->clips.end()) return INVALID_SOUND;

    SoundHandle handle = next_sound_id++;

    impl->sounds.emplace_back();
    ActiveSound& active = impl->sounds.back();
    active.handle = handle;
    active.clip_id = clip_id;
    active.group = params.group;
    active.spatial = true;
    active.base_volume = params.volume;
    active.pitch = params.pitch;
    active.position = position;
    active.sound = std::make_unique<ma_sound>();

    ma_result result = ma_sound_init_from_file(
        &impl->engine,
        it->second.path.c_str(),
        MA_SOUND_FLAG_DECODE,
        nullptr, nullptr,
        active.sound.get());

    if (result != MA_SUCCESS)
    {
        LOG_ENGINE_ERROR("Failed to play 3D sound '{}': {}", it->second.path, static_cast<int>(result));
        impl->sounds.pop_back();
        return INVALID_SOUND;
    }

    // Configure
    float effective_volume = params.volume * group_volumes[static_cast<int>(params.group)]
                           * group_volumes[static_cast<int>(AudioGroup::Master)];
    ma_sound_set_volume(active.sound.get(), effective_volume);
    ma_sound_set_pitch(active.sound.get(), params.pitch);
    ma_sound_set_looping(active.sound.get(), params.loop);

    // Spatial settings
    ma_sound_set_spatialization_enabled(active.sound.get(), MA_TRUE);
    ma_sound_set_position(active.sound.get(), position.x, position.y, position.z);
    ma_sound_set_min_distance(active.sound.get(), params.min_distance);
    ma_sound_set_max_distance(active.sound.get(), params.max_distance);
    ma_sound_set_attenuation_model(active.sound.get(), ma_attenuation_model_inverse);

    ma_sound_start(active.sound.get());
    active.active = true;

    return handle;
}

void AudioSystem::stopSound(SoundHandle handle)
{
    for (auto& sound : impl->sounds)
    {
        if (sound.handle == handle && sound.active)
        {
            ma_sound_stop(sound.sound.get());
            ma_sound_uninit(sound.sound.get());
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
            ma_sound_stop(sound.sound.get());
            ma_sound_uninit(sound.sound.get());
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
            return ma_sound_is_playing(sound.sound.get());
        }
    }
    return false;
}

void AudioSystem::setListenerPosition(const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up)
{
    if (std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z)) { listener_position = position; has_listener = true; }
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
    if (!std::isfinite(volume)) return;
    group_volumes[static_cast<int>(group)] = volume;
    if (!initialized) return;
    for (auto& s : impl->sounds) { if (!s.active) continue; float eff = s.base_volume * group_volumes[static_cast<int>(s.group)] * group_volumes[static_cast<int>(AudioGroup::Master)]; ma_sound_set_volume(s.sound.get(), eff); }
}

float AudioSystem::getGroupVolume(AudioGroup group) const
{
    return group_volumes[static_cast<int>(group)];
}

bool AudioSystem::setSoundPosition(SoundHandle handle, const glm::vec3& position)
{
    if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z)) return false;
    if (!initialized) return false;
    for (auto& s : impl->sounds) if (s.handle == handle && s.active) { ma_sound_set_position(s.sound.get(), position.x, position.y, position.z); s.position = position; return true; }
    return false;
}
bool AudioSystem::setSoundVolume(SoundHandle handle, float volume)
{
    if (!std::isfinite(volume)) return false;
    if (!initialized) return false;
    for (auto& s : impl->sounds) if (s.handle == handle && s.active) { s.base_volume = volume; float eff = volume * group_volumes[static_cast<int>(s.group)] * group_volumes[static_cast<int>(AudioGroup::Master)]; ma_sound_set_volume(s.sound.get(), eff); return true; }
    return false;
}
bool AudioSystem::setSoundPitch(SoundHandle handle, float pitch)
{
    if (!std::isfinite(pitch) || pitch <= 0.0f) return false;
    if (!initialized) return false;
    for (auto& s : impl->sounds) if (s.handle == handle && s.active) { s.pitch = pitch; ma_sound_set_pitch(s.sound.get(), pitch); return true; }
    return false;
}
bool AudioSystem::setSoundPaused(SoundHandle handle, bool paused)
{
    if (!initialized) return false;
    for (auto& s : impl->sounds) if (s.handle == handle && s.active) { s.paused = paused; if (paused) ma_sound_stop(s.sound.get()); else ma_sound_start(s.sound.get()); return true; }
    return false;
}
size_t AudioSystem::activeVoiceCount() const { size_t n = 0; for (auto& s : impl->sounds) if (s.active) ++n; return n; }
glm::vec3 AudioSystem::getSoundPosition(SoundHandle handle) const { for (auto& s : impl->sounds) if (s.handle == handle && s.active) return s.position; return glm::vec3(0.0f); }
float AudioSystem::getSoundVolume(SoundHandle handle) const { for (auto& s : impl->sounds) if (s.handle == handle && s.active) return s.base_volume; return 0.0f; }

void AudioSystem::update()
{
    if (!initialized) return;

    // Clean up finished (non-looping) sounds
    for (auto it = impl->sounds.begin(); it != impl->sounds.end(); )
    {
        if (it->active && !ma_sound_is_playing(it->sound.get()) && !ma_sound_is_looping(it->sound.get()))
        {
            ma_sound_uninit(it->sound.get());
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
