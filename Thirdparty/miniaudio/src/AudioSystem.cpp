#include "AudioSystem.h"

using namespace TE;

void AudioEventSystem::triggerPositionalEvent(const std::string& eventName, float x, float y, float z)
{
    if (!audioSystem) return;
    
    auto it = eventSoundMap.find(eventName);
    if (it != eventSoundMap.end() && !it->second.empty()) {
        // Get a random sound from the event's sound list
        size_t index = rand() % it->second.size();
        std::string soundId = it->second[index];
        
        // Get the config for this event-sound pair
        std::string configKey = eventName + ":" + soundId;
        auto configIt = eventConfigMap.find(configKey);
        
        // Create a temporary emitter
        auto emitter = audioSystem->createEmitter("TempEvent");
        emitter->setPosition(x, y, z);
        
        if (configIt != eventConfigMap.end()) {
            // Play with specific config
            audioSystem->playSound(emitter, soundId, configIt->second);
        } else {
            // Play with default config
            audioSystem->playSound(emitter, soundId);
        }
    }
}