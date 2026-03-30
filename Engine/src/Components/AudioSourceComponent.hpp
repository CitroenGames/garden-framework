#pragma once

#include "Audio/AudioSystem.hpp"

struct AudioSourceComponent
{
    AudioClipId clip = INVALID_AUDIO_CLIP;
    float volume = 1.0f;
    float pitch = 1.0f;
    float min_distance = 1.0f;
    float max_distance = 50.0f;
    bool spatial = true;
    bool loop = false;
    bool auto_play = false;
    AudioGroup group = AudioGroup::SFX;

    // Runtime state
    SoundHandle active_handle = INVALID_SOUND;
    bool started = false;
};
