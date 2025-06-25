#include "miniaudio.c"
#include <iostream>
#include <cmath>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <queue>
#include <future>
#include <functional>
#include <unordered_map>
#include <condition_variable>
#include <vector>
#include <memory>

#ifdef WIN32
    #undef min
    #undef max
#endif

#define M_PI       3.14159265358979323846   // pi

namespace TE {
    // Forward declarations
    class AudioEmitter;
    class AudioListener;
    
    // Audio event system
    enum class AudioEventType {
        Play,
        Stop,
        Pause,
        Resume,
        VolumeChange,
        PitchChange,
        PositionChange,
        LoopingChange
    };

    struct AudioEvent {
        AudioEventType type;
        std::string soundId;
        float paramFloat1 = 0.0f;
        float paramFloat2 = 0.0f;
        float paramFloat3 = 0.0f;
        bool paramBool = false;
    };

    // Sound types for different handling
    enum class SoundType {
        SFX,        // Short sound effects, use pooling
        Music,      // Background music, streaming
        Ambient,    // Environmental sounds, looping
        Voice       // Voice/dialogue, prioritized
    };

    // Sound configuration parameters
    struct SoundConfig {
        SoundType type = SoundType::SFX;
        bool loop = false;
        bool spatial = true;
        float volume = 1.0f;
        float pitch = 1.0f;
        float minDistance = 1.0f;
        float maxDistance = 100.0f;
        float priority = 0.5f;
    };

    // Sound instance representing a playing sound
    class SoundInstance {
    private:
        ma_sound* sound;
        std::string soundId;
        SoundConfig config;
        bool inUse;


        
    public:

        void updateDistanceEffects(float distance) {
            if (!sound || !config.spatial) return;

            // Calculate attenuation based on distance
            float attenuation = 1.0f;
            if (distance > config.minDistance) {
                if (distance >= config.maxDistance) {
                    attenuation = 0.0f;
                }
                else {
                    // Inverse distance squared attenuation
                    float range = config.maxDistance - config.minDistance;
                    float t = (distance - config.minDistance) / range;
                    attenuation = 1.0f - (t * t);
                }
            }

            // Apply volume attenuation
            ma_sound_set_volume(sound, config.volume * attenuation);

            // Optional: Apply low-pass filter based on distance
            // If miniaudio supports filters, we could do something like:
            // ma_sound_set_filter_cutoff(sound, 22000.0f - 20000.0f * (distance / config.maxDistance));
        }

        SoundInstance() : sound(nullptr), inUse(false) {}
        
        void initialize(ma_engine* engine, const std::string& filename, const SoundConfig& cfg) {
            if (sound) {
                ma_sound_uninit(sound);
                delete sound;
            }
            
            sound = new ma_sound();
            soundId = filename;
            config = cfg;
            
            ma_uint32 flags = 0;
            if (config.type == SoundType::Music) flags |= MA_SOUND_FLAG_STREAM;
            
            ma_sound_init_from_file(engine, filename.c_str(), flags, nullptr, nullptr, sound);
            
            ma_sound_set_volume(sound, config.volume);
            ma_sound_set_pitch(sound, config.pitch);
            ma_sound_set_looping(sound, config.loop);
            
            if (config.spatial) {
                ma_sound_set_min_distance(sound, config.minDistance);
                ma_sound_set_max_distance(sound, config.maxDistance);
            }
        }
        
        ~SoundInstance() {
            if (sound) {
                ma_sound_uninit(sound);
                delete sound;
                sound = nullptr;
            }
        }
        
        void play() {
            if (sound) ma_sound_start(sound);
            inUse = true;
        }
        
        void stop() {
            if (sound) ma_sound_stop(sound);
            inUse = false;
        }
        
        void pause() {
            if (sound) ma_sound_stop(sound);
        }
        
        void resume() {
            if (sound) ma_sound_start(sound);
        }
        
        bool isFinished() const {
            if (!sound) return true;
            return !ma_sound_is_playing(sound);
        }
        
        void setPosition(float x, float y, float z) {
            if (sound && config.spatial) {
                ma_sound_set_position(sound, x, y, z);
            }
        }
        
        void setVolume(float volume) {
            if (sound) {
                config.volume = volume;
                ma_sound_set_volume(sound, volume);
            }
        }
        
        void setPitch(float pitch) {
            if (sound) {
                config.pitch = pitch;
                ma_sound_set_pitch(sound, pitch);
            }
        }
        
        void setLooping(bool loop) {
            if (sound) {
                config.loop = loop;
                ma_sound_set_looping(sound, loop);
            }
        }
        
        float getPriority() const { return config.priority; }
        bool isInUse() const { return inUse; }
        ma_sound* getSound() const { return sound; }
        SoundType getType() const { return config.type; }
        std::string getSoundId() const { return soundId; }
    };

    // Audio emitter component for game objects
    class AudioEmitter {
    private:
        std::string name;
        float position[3] = {0, 0, 0};
        float velocity[3] = {0, 0, 0};
        float radius = 1.0f;
        float volume = 1.0f;
        bool active = true;
        std::vector<std::shared_ptr<SoundInstance>> attachedSounds;
        
    public:
        AudioEmitter(const std::string& emitterName = "Emitter") : name(emitterName) {}
        
        void setPosition(float x, float y, float z) {
            position[0] = x;
            position[1] = y;
            position[2] = z;
            
            // Update all attached sounds
            for (auto& sound : attachedSounds) {
                sound->setPosition(x, y, z);
            }
        }
        
        void setVelocity(float x, float y, float z) {
            velocity[0] = x;
            velocity[1] = y;
            velocity[2] = z;
        }
        
        void setRadius(float r) { radius = r; }
        void setVolume(float vol) { 
            volume = vol;
            for (auto& sound : attachedSounds) {
                sound->setVolume(vol);
            }
        }
        void setActive(bool isActive) { active = isActive; }

    private:
        float rolloffFactor = 1.0f;

    public:
        void setRolloffFactor(float factor) {
            rolloffFactor = factor;

            // Apply to all attached sounds
            for (auto& sound : attachedSounds) {
                // If miniaudio exposes this functionality:
                if (sound->getSound()) {
                    ma_sound_set_rolloff(sound->getSound(), rolloffFactor);
                }
            }
        }

        float getRolloffFactor() const { return rolloffFactor; }
        
        const float* getPosition() const { return position; }
        const float* getVelocity() const { return velocity; }
        float getRadius() const { return radius; }
        float getVolume() const { return volume; }
        bool isActive() const { return active; }
        const std::string& getName() const { return name; }
        
        void attachSound(std::shared_ptr<SoundInstance> sound) {
            if (sound) {
                attachedSounds.push_back(sound);
                sound->setPosition(position[0], position[1], position[2]);
            }
        }
        
        void detachSound(std::shared_ptr<SoundInstance> sound) {
            auto it = std::find(attachedSounds.begin(), attachedSounds.end(), sound);
            if (it != attachedSounds.end()) {
                attachedSounds.erase(it);
            }
        }
        
        void clearSounds() {
            attachedSounds.clear();
        }
    };

    // Audio listener component
    class AudioListener {
    private:
        float position[3] = {0, 0, 0};
        float velocity[3] = {0, 0, 0};
        float direction[3] = {0, 0, -1};
        float up[3] = {0, 1, 0};
        
    public:
        void setPosition(float x, float y, float z) {
            position[0] = x;
            position[1] = y;
            position[2] = z;
        }
        
        void setVelocity(float x, float y, float z) {
            velocity[0] = x;
            velocity[1] = y;
            velocity[2] = z;
        }
        
        void setDirection(float x, float y, float z) {
            direction[0] = x;
            direction[1] = y;
            direction[2] = z;
        }
        
        void setUp(float x, float y, float z) {
            up[0] = x;
            up[1] = y;
            up[2] = z;
        }
        
        void setOrientation(float yaw, float pitch, float roll = 0.0f) {
            // Convert degrees to radians
            float yawRad = yaw * (M_PI / 180.0f);
            float pitchRad = pitch * (M_PI / 180.0f);
            float rollRad = roll * (M_PI / 180.0f);

            // Calculate direction vector (forward)
            direction[0] = std::sin(yawRad) * std::cos(pitchRad);
            direction[1] = std::sin(pitchRad);
            direction[2] = std::cos(yawRad) * std::cos(pitchRad);
            
            // Calculate up vector (accounting for roll)
            float ux = std::sin(rollRad) * std::cos(yawRad);
            float uy = std::cos(rollRad);
            float uz = std::sin(rollRad) * std::sin(yawRad);
            
            up[0] = ux;
            up[1] = uy;
            up[2] = uz;
        }
        
        const float* getPosition() const { return position; }
        const float* getVelocity() const { return velocity; }
        const float* getDirection() const { return direction; }
        const float* getUp() const { return up; }
    };

    // Main audio system class
    class AudioSystem {
    private:
        ma_engine engine;
        bool engineInitialized = false;
        
        // Sound pools for efficient reuse
        std::vector<std::shared_ptr<SoundInstance>> sfxPool;
        size_t maxSfxPoolSize = 32;  // Maximum number of sound effects
        size_t maxConcurrentSounds = 16;  // Maximum number of simultaneous sounds
        size_t maxConcurrentMusic = 2;  // Maximum simultaneous music tracks (for crossfading)
        
        // Sound asset cache
        std::unordered_map<std::string, ma_sound*> soundTemplates;
        std::mutex cacheMutex;
        
        // Active sound instances
        std::vector<std::shared_ptr<SoundInstance>> activeSounds;
        std::vector<std::shared_ptr<SoundInstance>> streamingSounds;  // Music and long ambient
        std::mutex soundsMutex;
        
        // Event system
        std::queue<AudioEvent> eventQueue;
        std::mutex eventMutex;
        std::condition_variable eventCondition;
        
        // Thread management
        std::atomic<bool> running{false};
        std::thread audioThread;
        std::thread loaderThread;
        std::atomic<bool> loaderRunning{false};
        
        // Loading queue
        struct AssetLoadRequest {
            std::string filename;
            SoundConfig config;
            std::promise<ma_sound*> promise;
        };
        std::queue<AssetLoadRequest> loadQueue;
        std::mutex loadQueueMutex;
        std::condition_variable loadCondition;
        
        // Audio emitters and listeners
        std::vector<std::shared_ptr<AudioEmitter>> emitters;
        AudioListener listener;
        std::mutex emittersMutex;
        
        // Audio settings
        float masterVolume = 1.0f;
        float sfxVolume = 1.0f;
        float musicVolume = 1.0f;
        float voiceVolume = 1.0f;
        float ambientVolume = 1.0f;
        bool muteWhenFocusLost = true;
        
    public:
        AudioSystem() {
            std::cout << "AudioSystem constructor completed" << std::endl;
        }
        
        ~AudioSystem() {
            std::cout << "AudioSystem destructor called" << std::endl;
            cleanup();
        }
        
        bool initialize() {
            std::cout << "Initializing audio engine..." << std::endl;
            ma_result result;
            
            // Initialize engine
            ma_engine_config engineConfig = ma_engine_config_init();
            engineConfig.channels = 2;
            engineConfig.sampleRate = 48000;
            
            result = ma_engine_init(&engineConfig, &engine);
            if (result != MA_SUCCESS) {
                std::cerr << "Failed to initialize audio engine." << std::endl;
                return false;
            }
            engineInitialized = true;
            
            // Initialize sound pools
            initializeSoundPools();
            
            // Start threads
            startAudioThread();
            startAssetLoader();
            
            // Set initial listener properties
            updateListener();
            
            std::cout << "AudioSystem initialized successfully." << std::endl;
            return true;
        }
        
        void start() {
            if (!engineInitialized) {
                std::cerr << "Cannot start: Engine not initialized." << std::endl;
                return;
            }
            
            if (running.load()) {
                std::cout << "Audio system already running." << std::endl;
                return;
            }
            
            running = true;
            std::cout << "Audio system started." << std::endl;
        }
        
        void stop() {
            if (!running.load()) {
                std::cout << "Audio system already stopped." << std::endl;
                return;
            }
            
            running = false;
            
            // Stop all sounds
            {
                std::lock_guard<std::mutex> lock(soundsMutex);
                for (auto& sound : activeSounds) {
                    sound->stop();
                }
                for (auto& sound : streamingSounds) {
                    sound->stop();
                }
            }
            
            std::cout << "Audio system stopped." << std::endl;
        }
        
        void cleanup() {
            std::cout << "Cleaning up audio system..." << std::endl;
            stop();
            
            // Stop threads
            stopAudioThread();
            stopAssetLoader();
            
            // Clear all sounds
            {
                std::lock_guard<std::mutex> lock(soundsMutex);
                activeSounds.clear();
                streamingSounds.clear();
            }
            
            // Clear sound pools
            clearSoundPools();
            
            // Clear sound templates
            {
                std::lock_guard<std::mutex> lock(cacheMutex);
                for (auto& [name, sound] : soundTemplates) {
                    if (sound) {
                        ma_sound_uninit(sound);
                        delete sound;
                    }
                }
                soundTemplates.clear();
            }
            
            // Clear emitters
            {
                std::lock_guard<std::mutex> lock(emittersMutex);
                emitters.clear();
            }
            
            // Uninitialize engine
            if (engineInitialized) {
                ma_engine_uninit(&engine);
                engineInitialized = false;
            }
            
            std::cout << "Audio system cleanup complete." << std::endl;
        }
        
        // Sound management methods
        std::shared_ptr<SoundInstance> playSound(const std::string& filename, const SoundConfig& config = SoundConfig()) {
            AudioEvent event;
            event.type = AudioEventType::Play;
            event.soundId = filename;
            event.paramFloat1 = config.volume;
            event.paramFloat2 = config.pitch;
            event.paramBool = config.loop;
            
            queueEvent(event);
            
            // For immediate feedback, create and return a sound instance
            return createSoundInstance(filename, config);
        }
        
        void stopSound(const std::string& soundId) {
            AudioEvent event;
            event.type = AudioEventType::Stop;
            event.soundId = soundId;
            queueEvent(event);
        }
        
        void pauseSound(const std::string& soundId) {
            AudioEvent event;
            event.type = AudioEventType::Pause;
            event.soundId = soundId;
            queueEvent(event);
        }
        
        void resumeSound(const std::string& soundId) {
            AudioEvent event;
            event.type = AudioEventType::Resume;
            event.soundId = soundId;
            queueEvent(event);
        }
        
        void setVolume(const std::string& soundId, float volume) {
            AudioEvent event;
            event.type = AudioEventType::VolumeChange;
            event.soundId = soundId;
            event.paramFloat1 = volume;
            queueEvent(event);
        }
        
        void setPitch(const std::string& soundId, float pitch) {
            AudioEvent event;
            event.type = AudioEventType::PitchChange;
            event.soundId = soundId;
            event.paramFloat1 = pitch;
            queueEvent(event);
        }
        
        // Listener control
        void setListenerPosition(float x, float y, float z) {
            listener.setPosition(x, y, z);
            updateListener();
        }
        
        void setListenerDirection(float x, float y, float z) {
            listener.setDirection(x, y, z);
            updateListener();
        }
        
        void setListenerUp(float x, float y, float z) {
            listener.setUp(x, y, z);
            updateListener();
        }
        
        void setListenerOrientation(float yaw, float pitch, float roll = 0.0f) {
            listener.setOrientation(yaw, pitch, roll);
            updateListener();
        }
        
        void setListenerVelocity(float x, float y, float z) {
            listener.setVelocity(x, y, z);
            updateListener();
        }
        
        // Emitter management
        std::shared_ptr<AudioEmitter> createEmitter(const std::string& name = "Emitter") {
            auto emitter = std::make_shared<AudioEmitter>(name);
            
            {
                std::lock_guard<std::mutex> lock(emittersMutex);
                emitters.push_back(emitter);
            }
            
            return emitter;
        }
        
        void removeEmitter(std::shared_ptr<AudioEmitter> emitter) {
            if (!emitter) return;
            
            std::lock_guard<std::mutex> lock(emittersMutex);
            auto it = std::find(emitters.begin(), emitters.end(), emitter);
            if (it != emitters.end()) {
                emitters.erase(it);
            }
        }
        
        // Play sound attached to an emitter
        std::shared_ptr<SoundInstance> playSound(std::shared_ptr<AudioEmitter> emitter, 
                                                 const std::string& filename, 
                                                 const SoundConfig& config = SoundConfig()) {
            if (!emitter) return nullptr;
            
            auto sound = createSoundInstance(filename, config);
            if (sound) {
                const float* pos = emitter->getPosition();
                sound->setPosition(pos[0], pos[1], pos[2]);
                emitter->attachSound(sound);
                sound->play();
            }
            
            return sound;
        }
        
        // Volume control
        void setMasterVolume(float volume) {
            masterVolume = std::max(0.0f, std::min(1.0f, volume));
            ma_engine_set_volume(&engine, masterVolume);
        }
        
        void setSfxVolume(float volume) {
            sfxVolume = std::max(0.0f, std::min(1.0f, volume));
            updateCategoryVolumes();
        }
        
        void setMusicVolume(float volume) {
            musicVolume = std::max(0.0f, std::min(1.0f, volume));
            updateCategoryVolumes();
        }
        
        void setVoiceVolume(float volume) {
            voiceVolume = std::max(0.0f, std::min(1.0f, volume));
            updateCategoryVolumes();
        }
        
        void setAmbientVolume(float volume) {
            ambientVolume = std::max(0.0f, std::min(1.0f, volume));
            updateCategoryVolumes();
        }
        
        // Preload sounds
        void preloadSound(const std::string& filename, const SoundConfig& config = SoundConfig()) {
            loadSoundAsync(filename, config);
        }
        
        // Focus handling for game window
        void onFocusLost() {
            if (muteWhenFocusLost) {
                ma_engine_set_volume(&engine, 0.0f);
            }
        }
        
        void onFocusGained() {
            if (muteWhenFocusLost) {
                ma_engine_set_volume(&engine, masterVolume);
            }
        }
        
        // Music control with crossfading
        void playMusic(const std::string& filename, float fadeInTime = 2.0f, float fadeOutTime = 2.0f) {
            SoundConfig config;
            config.type = SoundType::Music;
            config.loop = true;
            config.spatial = false;
            config.volume = 0.0f;  // Start at zero for fade-in
            
            auto music = createSoundInstance(filename, config);
            if (music) {
                // Fade out existing music
                std::lock_guard<std::mutex> lock(soundsMutex);
                for (auto& sound : streamingSounds) {
                    if (sound->getType() == SoundType::Music) {
                        fadeOut(sound, fadeOutTime);
                    }
                }
                
                // Add new music and fade in
                streamingSounds.push_back(music);
                music->play();
                fadeIn(music, fadeInTime);
            }
        }
        
        void stopMusic(float fadeOutTime = 2.0f) {
            std::lock_guard<std::mutex> lock(soundsMutex);
            for (auto& sound : streamingSounds) {
                if (sound->getType() == SoundType::Music) {
                    fadeOut(sound, fadeOutTime);
                }
            }
        }
        
        // Sound grouping for game systems
        void playAmbience(const std::string& filename, float fadeInTime = 3.0f) {
            SoundConfig config;
            config.type = SoundType::Ambient;
            config.loop = true;
            config.spatial = true;
            config.minDistance = 5.0f;
            config.maxDistance = 50.0f;
            config.volume = 0.0f;  // Start at zero for fade-in
            
            auto ambient = createSoundInstance(filename, config);
            if (ambient) {
                std::lock_guard<std::mutex> lock(soundsMutex);
                streamingSounds.push_back(ambient);
                ambient->play();
                fadeIn(ambient, fadeInTime);
            }
        }
        
        // Voice dialogue with priority
        void playVoice(const std::string& filename, float priority = 1.0f) {
            SoundConfig config;
            config.type = SoundType::Voice;
            config.loop = false;
            config.spatial = false;
            config.priority = priority;
            
            // Stop lower priority voices
            {
                std::lock_guard<std::mutex> lock(soundsMutex);
                for (auto it = activeSounds.begin(); it != activeSounds.end();) {
                    if ((*it)->getType() == SoundType::Voice && (*it)->getPriority() < priority) {
                        (*it)->stop();
                        it = activeSounds.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            
            // Play new voice
            auto voice = createSoundInstance(filename, config);
            if (voice) {
                std::lock_guard<std::mutex> lock(soundsMutex);
                activeSounds.push_back(voice);
                voice->play();
            }
        }
        
    private:
        // Initialize sound pools
        void initializeSoundPools() {
            sfxPool.reserve(maxSfxPoolSize);
            for (size_t i = 0; i < maxSfxPoolSize; ++i) {
                sfxPool.push_back(std::make_shared<SoundInstance>());
            }
        }
        
        void clearSoundPools() {
            sfxPool.clear();
        }
        
        // Find available sound from pool
        std::shared_ptr<SoundInstance> getAvailableSoundFromPool(SoundType type) {
            if (type == SoundType::SFX) {
                for (auto& sound : sfxPool) {
                    if (!sound->isInUse() || sound->isFinished()) {
                        return sound;
                    }
                }
                
                // If all are in use, find the oldest non-playing sound
                for (auto& sound : sfxPool) {
                    if (!ma_sound_is_playing(sound->getSound())) {
                        return sound;
                    }
                }
                
                // If all are playing, return the first one (will be recycled)
                return sfxPool[0];
            }
            
            // For non-pooled types, create a new instance
            return std::make_shared<SoundInstance>();
        }
        
        // Create a sound instance
        std::shared_ptr<SoundInstance> createSoundInstance(const std::string& filename, const SoundConfig& config) {
            std::shared_ptr<SoundInstance> instance;
            
            // Get sound from pool for SFX
            if (config.type == SoundType::SFX) {
                instance = getAvailableSoundFromPool(SoundType::SFX);
            } else {
                instance = std::make_shared<SoundInstance>();
            }
            
            // Initialize the sound
            instance->initialize(&engine, filename, config);
            
            // Add to active sounds list if not an SFX (which are managed in the pool)
            if (config.type != SoundType::SFX) {
                std::lock_guard<std::mutex> lock(soundsMutex);
                
                // Enforce limits on concurrent sounds
                enforceSoundLimits(config.type);
                
                if (config.type == SoundType::Music || config.type == SoundType::Ambient) {
                    streamingSounds.push_back(instance);
                } else {
                    activeSounds.push_back(instance);
                }
            }
            
            return instance;
        }
        
        // Enforce limits on number of concurrent sounds
        void enforceSoundLimits(SoundType type) {
            if (type == SoundType::Music) {
                // Limit music tracks
                while (streamingSounds.size() >= maxConcurrentMusic) {
                    // Find non-playing or oldest music
                    auto it = std::find_if(streamingSounds.begin(), streamingSounds.end(), 
                                          [](const std::shared_ptr<SoundInstance>& sound) {
                                              return sound->getType() == SoundType::Music && !sound->isInUse();
                                          });
                    
                    if (it != streamingSounds.end()) {
                        streamingSounds.erase(it);
                    } else if (!streamingSounds.empty()) {
                        streamingSounds.erase(streamingSounds.begin());
                    }
                }
            } else if (type == SoundType::SFX) {
                // SFX are managed by the pool
                return;
            } else {
                // Limit active sounds
                while (activeSounds.size() >= maxConcurrentSounds) {
                    // Find lowest priority sound
                    float lowestPriority = 2.0f;
                    auto lowestIt = activeSounds.end();
                    
                    for (auto it = activeSounds.begin(); it != activeSounds.end(); ++it) {
                        if ((*it)->getPriority() < lowestPriority) {
                            lowestPriority = (*it)->getPriority();
                            lowestIt = it;
                        }
                    }
                    
                    if (lowestIt != activeSounds.end()) {
                        (*lowestIt)->stop();
                        activeSounds.erase(lowestIt);
                    } else if (!activeSounds.empty()) {
                        // If no low priority sound found, remove the first one
                        activeSounds.erase(activeSounds.begin());
                    }
                }
            }
        }
        
        // Fade effects
        void fadeIn(std::shared_ptr<SoundInstance> sound, float timeSeconds) {
            if (!sound || timeSeconds <= 0) return;
            
            float startVolume = 0.0f;
            float endVolume = sound->getType() == SoundType::Music ? musicVolume : 
                             (sound->getType() == SoundType::Ambient ? ambientVolume : 1.0f);
            
            // Start a fade thread
            std::thread fadeThread([sound, startVolume, endVolume, timeSeconds]() {
                const int steps = 50;
                float stepTime = timeSeconds / steps;
                
                for (int i = 0; i <= steps; ++i) {
                    float progress = static_cast<float>(i) / steps;
                    float volume = startVolume + (endVolume - startVolume) * progress;
                    
                    sound->setVolume(volume);
                    std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(stepTime * 1000)));
                }
            });
            
            fadeThread.detach();
        }
        
        void fadeOut(std::shared_ptr<SoundInstance> sound, float timeSeconds) {
            if (!sound || timeSeconds <= 0) return;
            
            float startVolume = sound->getType() == SoundType::Music ? musicVolume : 
                               (sound->getType() == SoundType::Ambient ? ambientVolume : 1.0f);
            float endVolume = 0.0f;
            
            // Start a fade thread
            std::thread fadeThread([this, sound, startVolume, endVolume, timeSeconds]() {
                const int steps = 50;
                float stepTime = timeSeconds / steps;
                
                for (int i = 0; i <= steps; ++i) {
                    float progress = static_cast<float>(i) / steps;
                    float volume = startVolume + (endVolume - startVolume) * progress;
                    
                    sound->setVolume(volume);
                    std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(stepTime * 1000)));
                }
                
                // When fade complete, stop the sound
                sound->stop();
                
                // Remove from active lists
                std::lock_guard<std::mutex> lock(soundsMutex);
                
                auto it = std::find(activeSounds.begin(), activeSounds.end(), sound);
                if (it != activeSounds.end()) {
                    activeSounds.erase(it);
                }
                
                auto it2 = std::find(streamingSounds.begin(), streamingSounds.end(), sound);
                if (it2 != streamingSounds.end()) {
                    streamingSounds.erase(it2);
                }
            });
            
            fadeThread.detach();
        }
        
        // Update volumes based on sound categories
        void updateCategoryVolumes() {
            std::lock_guard<std::mutex> lock(soundsMutex);
            
            // Update active sounds
            for (auto& sound : activeSounds) {
                float categoryVolume = 1.0f;
                switch (sound->getType()) {
                    case SoundType::SFX:
                        categoryVolume = sfxVolume;
                        break;
                    case SoundType::Voice:
                        categoryVolume = voiceVolume;
                        break;
                    case SoundType::Music:
                        categoryVolume = musicVolume;
                        break;
                    case SoundType::Ambient:
                        categoryVolume = ambientVolume;
                        break;
                }
                
                sound->setVolume(categoryVolume);
            }
            
            // Update streaming sounds
            for (auto& sound : streamingSounds) {
                float categoryVolume = 1.0f;
                switch (sound->getType()) {
                    case SoundType::Music:
                        categoryVolume = musicVolume;
                        break;
                    case SoundType::Ambient:
                        categoryVolume = ambientVolume;
                        break;
                    default:
                        break;
                }
                
                sound->setVolume(categoryVolume);
            }
        }
        
        // Audio thread
        void startAudioThread() {
            if (running.load()) return;
            
            running = true;
            audioThread = std::thread([this]() {
                std::cout << "Audio processing thread started" << std::endl;
                while (running.load()) {
                    // Process events
                    processEvents();
                    
                    // Update sounds
                    updateSounds();
                    
                    // Sleep to reduce CPU usage
                    std::this_thread::sleep_for(std::chrono::milliseconds(16)); // ~60 updates per second
                }
                std::cout << "Audio processing thread stopped" << std::endl;
            });
        }
        
        void stopAudioThread() {
            if (!running.load()) return;
            
            running = false;
            if (audioThread.joinable()) {
                audioThread.join();
            }
        }
        
        // Process audio events
        void processEvents() {
            std::queue<AudioEvent> events;
            
            // Get all pending events
            {
                std::lock_guard<std::mutex> lock(eventMutex);
                std::swap(events, eventQueue);
            }
            
            // Process events
            while (!events.empty()) {
                AudioEvent& event = events.front();
                handleEvent(event);
                events.pop();
            }
        }
        
        void handleEvent(const AudioEvent& event) {
            switch (event.type) {
                case AudioEventType::Play: {
                    SoundConfig config;
                    config.volume = event.paramFloat1;
                    config.pitch = event.paramFloat2;
                    config.loop = event.paramBool;
                    createSoundInstance(event.soundId, config)->play();
                    break;
                }
                    
                case AudioEventType::Stop:
                    stopAllInstances(event.soundId);
                    break;
                    
                case AudioEventType::Pause:
                    pauseAllInstances(event.soundId);
                    break;
                    
                case AudioEventType::Resume:
                    resumeAllInstances(event.soundId);
                    break;
                    
                case AudioEventType::VolumeChange:
                    setVolumeAllInstances(event.soundId, event.paramFloat1);
                    break;
                    
                case AudioEventType::PitchChange:
                    setPitchAllInstances(event.soundId, event.paramFloat1);
                    break;
                    
                case AudioEventType::PositionChange:
                    setPositionAllInstances(event.soundId, 
                                           event.paramFloat1, 
                                           event.paramFloat2, 
                                           event.paramFloat3);
                    break;
                    
                case AudioEventType::LoopingChange:
                    setLoopingAllInstances(event.soundId, event.paramBool);
                    break;
            }
        }
        
        // Apply operations to all instances of a sound
        void stopAllInstances(const std::string& soundId) {
            std::lock_guard<std::mutex> lock(soundsMutex);
            
            for (auto it = activeSounds.begin(); it != activeSounds.end();) {
                if ((*it)->getSoundId() == soundId) {
                    (*it)->stop();
                    it = activeSounds.erase(it);
                } else {
                    ++it;
                }
            }
            
            for (auto it = streamingSounds.begin(); it != streamingSounds.end();) {
                if ((*it)->getSoundId() == soundId) {
                    (*it)->stop();
                    it = streamingSounds.erase(it);
                } else {
                    ++it;
                }
            }
        }
        
        void pauseAllInstances(const std::string& soundId) {
            std::lock_guard<std::mutex> lock(soundsMutex);
            
            for (auto& sound : activeSounds) {
                if (sound->getSoundId() == soundId) {
                    sound->pause();
                }
            }
            
            for (auto& sound : streamingSounds) {
                if (sound->getSoundId() == soundId) {
                    sound->pause();
                }
            }
        }
        
        void resumeAllInstances(const std::string& soundId) {
            std::lock_guard<std::mutex> lock(soundsMutex);
            
            for (auto& sound : activeSounds) {
                if (sound->getSoundId() == soundId) {
                    sound->resume();
                }
            }
            
            for (auto& sound : streamingSounds) {
                if (sound->getSoundId() == soundId) {
                    sound->resume();
                }
            }
        }
        
        void setVolumeAllInstances(const std::string& soundId, float volume) {
            std::lock_guard<std::mutex> lock(soundsMutex);
            
            for (auto& sound : activeSounds) {
                if (sound->getSoundId() == soundId) {
                    sound->setVolume(volume);
                }
            }
            
            for (auto& sound : streamingSounds) {
                if (sound->getSoundId() == soundId) {
                    sound->setVolume(volume);
                }
            }
        }
        
        void setPitchAllInstances(const std::string& soundId, float pitch) {
            std::lock_guard<std::mutex> lock(soundsMutex);
            
            for (auto& sound : activeSounds) {
                if (sound->getSoundId() == soundId) {
                    sound->setPitch(pitch);
                }
            }
            
            for (auto& sound : streamingSounds) {
                if (sound->getSoundId() == soundId) {
                    sound->setPitch(pitch);
                }
            }
        }
        
        void setPositionAllInstances(const std::string& soundId, float x, float y, float z) {
            std::lock_guard<std::mutex> lock(soundsMutex);
            
            for (auto& sound : activeSounds) {
                if (sound->getSoundId() == soundId) {
                    sound->setPosition(x, y, z);
                }
            }
            
            for (auto& sound : streamingSounds) {
                if (sound->getSoundId() == soundId) {
                    sound->setPosition(x, y, z);
                }
            }
        }
        
        void setLoopingAllInstances(const std::string& soundId, bool loop) {
            std::lock_guard<std::mutex> lock(soundsMutex);
            
            for (auto& sound : activeSounds) {
                if (sound->getSoundId() == soundId) {
                    sound->setLooping(loop);
                }
            }
            
            for (auto& sound : streamingSounds) {
                if (sound->getSoundId() == soundId) {
                    sound->setLooping(loop);
                }
            }
        }
        
        // Update all sounds (check for finished sounds, etc.)
        void updateSounds() {
            std::lock_guard<std::mutex> lock(soundsMutex);
            
            // Remove finished sounds from active list
            for (auto it = activeSounds.begin(); it != activeSounds.end();) {
                if ((*it)->isFinished()) {
                    it = activeSounds.erase(it);
                } else {
                    ++it;
                }
            }
            
            // Check streaming sounds (don't remove looping ones)
            for (auto it = streamingSounds.begin(); it != streamingSounds.end();) {
                if ((*it)->isFinished() && !ma_sound_is_looping((*it)->getSound())) {
                    it = streamingSounds.erase(it);
                } else {
                    ++it;
                }
            }
        }
        
        // Update listener properties in miniaudio
        void updateListener() {
            if (!engineInitialized) return;
            
            const float* pos = listener.getPosition();
            const float* dir = listener.getDirection();
            const float* up = listener.getUp();
            const float* vel = listener.getVelocity();
            
            ma_engine_listener_set_position(&engine, 0, pos[0], pos[1], pos[2]);
            ma_engine_listener_set_direction(&engine, 0, dir[0], dir[1], dir[2]);
            ma_engine_listener_set_world_up(&engine, 0, up[0], up[1], up[2]);
            ma_engine_listener_set_velocity(&engine, 0, vel[0], vel[1], vel[2]);
        }
        
        // Queue an audio event
        void queueEvent(const AudioEvent& event) {
            std::lock_guard<std::mutex> lock(eventMutex);
            eventQueue.push(event);
            eventCondition.notify_one();
        }
        
        // Asset loader thread
        void startAssetLoader() {
            if (loaderRunning.load()) return;
            
            loaderRunning = true;
            loaderThread = std::thread([this]() {
                std::cout << "Asset loader thread started" << std::endl;
                while (loaderRunning.load()) {
                    AssetLoadRequest request;
                    bool hasRequest = false;
                    
                    {
                        std::unique_lock<std::mutex> lock(loadQueueMutex);
                        
                        // Wait for load request or shutdown
                        loadCondition.wait(lock, [this]() {
                            return !loadQueue.empty() || !loaderRunning.load();
                        });
                        
                        // Check if shutting down
                        if (!loaderRunning.load() && loadQueue.empty()) {
                            break;
                        }
                        
                        // Get next request
                        if (!loadQueue.empty()) {
                            request = std::move(loadQueue.front());
                            loadQueue.pop();
                            hasRequest = true;
                        }
                    }
                    
                    // Process request
                    if (hasRequest) {
                        loadSoundTemplate(request);
                    }
                }
                std::cout << "Asset loader thread stopped" << std::endl;
            });
        }
        
        void stopAssetLoader() {
            if (!loaderRunning.load()) return;
            
            loaderRunning = false;
            loadCondition.notify_all();
            
            if (loaderThread.joinable()) {
                loaderThread.join();
            }
        }
        
        // Load a sound template (for caching)
        void loadSoundTemplate(AssetLoadRequest& request) {
            try {
                // Check if already in cache
                {
                    std::lock_guard<std::mutex> lock(cacheMutex);
                    auto it = soundTemplates.find(request.filename);
                    if (it != soundTemplates.end() && it->second != nullptr) {
                        // Already loaded
                        request.promise.set_value(it->second);
                        return;
                    }
                }
                
                // Create new sound
                ma_sound* sound = new ma_sound();
                
                // Determine flags
                ma_uint32 flags = 0;
                if (request.config.type == SoundType::Music || 
                    request.config.type == SoundType::Ambient) {
                    flags |= MA_SOUND_FLAG_STREAM;
                }
                
                // Load from file
                ma_result result = ma_sound_init_from_file(&engine, 
                                                          request.filename.c_str(), 
                                                          flags, 
                                                          nullptr, 
                                                          nullptr, 
                                                          sound);
                
                if (result == MA_SUCCESS) {
                    // Add to cache
                    {
                        std::lock_guard<std::mutex> lock(cacheMutex);
                        soundTemplates[request.filename] = sound;
                    }
                    
                    // Set result
                    request.promise.set_value(sound);
                } else {
                    std::cerr << "Failed to load sound: " << request.filename 
                              << ", error code: " << result << std::endl;
                    delete sound;
                    request.promise.set_exception(std::make_exception_ptr(
                        std::runtime_error("Failed to load sound: " + request.filename)));
                }
            } catch (const std::exception& e) {
                std::cerr << "Exception loading sound: " << e.what() << std::endl;
                request.promise.set_exception(std::current_exception());
            }
        }
        
        // Load sound asynchronously
        std::future<ma_sound*> loadSoundAsync(const std::string& filename, const SoundConfig& config = SoundConfig()) {
            // Check if already in cache
            {
                std::lock_guard<std::mutex> lock(cacheMutex);
                auto it = soundTemplates.find(filename);
                if (it != soundTemplates.end() && it->second != nullptr) {
                    // Return immediately
                    std::promise<ma_sound*> promise;
                    promise.set_value(it->second);
                    return promise.get_future();
                }
            }
            
            // Create load request
            AssetLoadRequest request;
            request.filename = filename;
            request.config = config;
            
            // Get future before pushing to queue
            std::future<ma_sound*> future = request.promise.get_future();
            
            // Add to load queue
            {
                std::lock_guard<std::mutex> lock(loadQueueMutex);
                loadQueue.push(std::move(request));
                loadCondition.notify_one();
            }
            
            return future;
        }
    };
    
    // AudioEmitterComponent for game entities
    class AudioEmitterComponent {
    private:
        std::shared_ptr<AudioEmitter> emitter;
        AudioSystem* audioSystem;
        
    public:
        AudioEmitterComponent(AudioSystem* system, const std::string& name = "EmitterComponent")
            : audioSystem(system) {
            if (audioSystem) {
                emitter = audioSystem->createEmitter(name);
            }
        }
        
        ~AudioEmitterComponent() {
            if (audioSystem && emitter) {
                audioSystem->removeEmitter(emitter);
            }
        }
        
        void setPosition(float x, float y, float z) {
            if (emitter) {
                emitter->setPosition(x, y, z);
            }
        }
        
        void setVelocity(float x, float y, float z) {
            if (emitter) {
                emitter->setVelocity(x, y, z);
            }
        }
        
        void setActive(bool active) {
            if (emitter) {
                emitter->setActive(active);
            }
        }
        
        void setVolume(float volume) {
            if (emitter) {
                emitter->setVolume(volume);
            }
        }
        
        void setRadius(float radius) {
            if (emitter) {
                emitter->setRadius(radius);
            }
        }
        
        std::shared_ptr<SoundInstance> playSound(const std::string& soundId, const SoundConfig& config = SoundConfig()) {
            if (audioSystem && emitter) {
                return audioSystem->playSound(emitter, soundId, config);
            }
            return nullptr;
        }
        
        std::shared_ptr<AudioEmitter> getEmitter() const {
            return emitter;
        }
    };
    
    // Audio listener component for player/camera
    class AudioListenerComponent {
    private:
        AudioSystem* audioSystem;
        
    public:
        AudioListenerComponent(AudioSystem* system) : audioSystem(system) {}
        
        void setPosition(float x, float y, float z) {
            if (audioSystem) {
                audioSystem->setListenerPosition(x, y, z);
            }
        }
        
        void setOrientation(float yaw, float pitch, float roll = 0.0f) {
            if (audioSystem) {
                audioSystem->setListenerOrientation(yaw, pitch, roll);
            }
        }
        
        void setVelocity(float x, float y, float z) {
            if (audioSystem) {
                audioSystem->setListenerVelocity(x, y, z);
            }
        }
    };
    
    // Audio events for game events
    class AudioEventSystem {
    private:
        AudioSystem* audioSystem;
        std::unordered_map<std::string, std::vector<std::string>> eventSoundMap;
        std::unordered_map<std::string, SoundConfig> eventConfigMap;
        
    public:
        AudioEventSystem(AudioSystem* system) : audioSystem(system) {}
        
        void registerEvent(const std::string& eventName, const std::string& soundId, const SoundConfig& config = SoundConfig()) {
            eventSoundMap[eventName].push_back(soundId);
            eventConfigMap[eventName + ":" + soundId] = config;
        }
        
        void triggerEvent(const std::string& eventName) {
            if (!audioSystem) return;
            
            auto it = eventSoundMap.find(eventName);
            if (it != eventSoundMap.end() && !it->second.empty()) {
                // Get a random sound from the event's sound list
                size_t index = rand() % it->second.size();
                std::string soundId = it->second[index];
                
                // Get the config for this event-sound pair
                std::string configKey = eventName + ":" + soundId;
                auto configIt = eventConfigMap.find(configKey);
                
                if (configIt != eventConfigMap.end()) {
                    // Play with specific config
                    audioSystem->playSound(soundId, configIt->second);
                } else {
                    // Play with default config
                    audioSystem->playSound(soundId);
                }
            }
        }
        
        void triggerPositionalEvent(const std::string& eventName, float x, float y, float z);
    };
}