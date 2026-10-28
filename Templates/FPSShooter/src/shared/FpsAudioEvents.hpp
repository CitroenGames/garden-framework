#pragma once
#include "Audio/SoundEvent.hpp"
#include "Audio/SoundEventTable.hpp"

// FPSShooter template-owned sound event table (data, not engine code).
// Clip files are optional: a missing clip makes the event a safe,
// rate-limited-log no-op in AudioEventSystem.
inline const SoundEventTable& fpsAudioEventTable()
{
    static const SoundEventTable table = [] {
        SoundEventTable result;
        const bool created = SoundEventTable::create(SoundEventTable::CurrentVersion,
            {
                {"weapon.fire", {"assets/audio/weapon_fire.wav"}, 1.0f, 0.95f, 1.05f, AudioGroup::SFX, 1.0f, 60.0f, 70, 6, false},
                {"player.footstep", {"assets/audio/footstep.wav"}, 0.5f, 0.9f, 1.1f, AudioGroup::SFX, 1.0f, 15.0f, 30, 4, false},
                {"player.hurt", {"assets/audio/player_hurt.wav"}, 0.8f, 0.95f, 1.05f, AudioGroup::SFX, 1.0f, 20.0f, 60, 2, false},
                {"player.death", {"assets/audio/player_death.wav"}, 0.9f, 0.95f, 1.05f, AudioGroup::SFX, 1.0f, 30.0f, 60, 2, false},
                {"ui.click", {"assets/audio/ui_click.wav"}, 0.7f, 1.0f, 1.0f, AudioGroup::UI, 1.0f, 1.0f, 80, 2, false},
            }, result);
        (void)created; // Static template data is validated at construction and by GameplayTests.
        return result;
    }();
    return table;
}

inline bool registerFpsAudioEvents()
{
    return AudioEventSystem::get().replaceEventTable(fpsAudioEventTable());
}
