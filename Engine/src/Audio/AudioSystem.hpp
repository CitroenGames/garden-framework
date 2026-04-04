#pragma once

#include "EngineExport.h"
#include <glm/glm.hpp>
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <memory>

// Opaque handle types
using AudioClipId = uint32_t;
using SoundHandle = uint32_t;
constexpr AudioClipId INVALID_AUDIO_CLIP = 0;
constexpr SoundHandle INVALID_SOUND = 0;

enum class AudioGroup : uint8_t
{
    Master = 0,
    SFX,
    Music,
    Voice,
    UI,
    COUNT
};

struct PlayParams
{
    float volume = 1.0f;
    float pitch = 1.0f;
    bool loop = false;
    AudioGroup group = AudioGroup::SFX;
};

struct Play3DParams
{
    float volume = 1.0f;
    float pitch = 1.0f;
    bool loop = false;
    AudioGroup group = AudioGroup::SFX;
    float min_distance = 1.0f;
    float max_distance = 50.0f;
};

class ENGINE_API AudioSystem
{
public:
    static AudioSystem& get()
    {
        static AudioSystem instance;
        return instance;
    }

    // Lifecycle
    bool initialize();
    void shutdown();

    // Clip management
    AudioClipId loadClip(const std::string& path);
    void unloadClip(AudioClipId id);
    bool isClipLoaded(AudioClipId id) const;

    // Playback - fire and forget
    SoundHandle playSound(AudioClipId clip, const PlayParams& params = {});

    // Spatial 3D playback
    SoundHandle playSound3D(AudioClipId clip, const glm::vec3& position, const Play3DParams& params = {});

    // Sound control
    void stopSound(SoundHandle handle);
    void stopAllSounds();
    bool isSoundPlaying(SoundHandle handle) const;

    // Listener (typically the camera/player)
    void setListenerPosition(const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up);

    // Volume control
    void setMasterVolume(float volume);
    float getMasterVolume() const { return group_volumes[static_cast<int>(AudioGroup::Master)]; }
    void setGroupVolume(AudioGroup group, float volume);
    float getGroupVolume(AudioGroup group) const;

    // Call each frame to update 3D positions and clean up finished sounds
    void update();

    bool isInitialized() const { return initialized; }

private:
    AudioSystem();
    ~AudioSystem();
    AudioSystem(const AudioSystem&) = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;

    struct AudioClip;
    struct ActiveSound;

    struct Impl;
    std::unique_ptr<Impl> impl;

    bool initialized = false;
    uint32_t next_clip_id = 1;
    uint32_t next_sound_id = 1;

    float group_volumes[static_cast<int>(AudioGroup::COUNT)] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
};
