#include "AudioSystem.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <cmath>

int main() {
    std::cout << "Starting audio approach and bounce demonstration..." << std::endl;

    // Create and initialize the audio system
    TE::AudioSystem audioSystem;

    if (!audioSystem.initialize()) {
        std::cerr << "Failed to initialize audio system." << std::endl;
        return -1;
    }

    std::cout << "Audio system initialized successfully." << std::endl;
    audioSystem.start();

    // Set up the audio listener (player)
    audioSystem.setListenerPosition(0.0f, 0.0f, 0.0f);    // Listener at center
    audioSystem.setListenerDirection(0.0f, 0.0f, -1.0f);  // Looking forward (negative Z)
    audioSystem.setListenerUp(0.0f, 1.0f, 0.0f);          // Up is positive Y

    // Configure sound parameters
    TE::SoundConfig config;
    config.type = TE::SoundType::SFX;      // SFX type for non-streaming sound
    config.loop = true;                    // Make it loop continuously
    config.spatial = true;                 // Enable spatial audio
    config.minDistance = 1.0f;             // Sound is at full volume within this distance
    config.maxDistance = 50.0f;            // Sound can be heard up to this distance
    config.volume = 1.0f;                  // Full volume

    // Create an emitter that will approach the listener
    auto approachEmitter = audioSystem.createEmitter("ApproachBounceSound");

    // Starting position - far away from listener
    float startDistance = 40.0f;          // Start near max distance
    float approachSpeed = 2.0f;           // Units per second
    float bounceSpeed = 8.0f;             // Speed after bounce (faster retreat)

    // Position the emitter far away to start
    approachEmitter->setPosition(0.0f, 0.0f, startDistance);

    std::cout << "Playing approaching sound..." << std::endl;
    auto soundInstance = audioSystem.playSound(approachEmitter, "audio_sample.mp3", config);

    if (!soundInstance) {
        std::cerr << "Failed to play sound!" << std::endl;
        audioSystem.cleanup();
        return -1;
    }

    std::cout << "Sound is starting far away and approaching the listener." << std::endl;
    std::cout << "Press Ctrl+C to exit." << std::endl;

    // Movement variables
    float currentDistance = startDistance;
    bool isApproaching = true;
    float bounceTriggerDistance = 5.0f;  // Distance at which to bounce away

    // Main loop
    bool running = true;
    auto lastUpdateTime = std::chrono::high_resolution_clock::now();

    while (running) {
        try {
            // Calculate time step
            auto currentTime = std::chrono::high_resolution_clock::now();
            float deltaTime = std::chrono::duration<float>(currentTime - lastUpdateTime).count();
            lastUpdateTime = currentTime;

            // Update position based on approach or bounce state
            if (isApproaching) {
                // Sound is approaching the listener
                currentDistance -= approachSpeed * deltaTime;

                // Check if we've reached the bounce point
                if (currentDistance <= bounceTriggerDistance) {
                    std::cout << "Sound is now bouncing away!" << std::endl;
                    isApproaching = false;
                    currentDistance = bounceTriggerDistance;
                }
            }
            else {
                // Sound is bouncing away from the listener
                currentDistance += bounceSpeed * deltaTime;

                // Check if we've reached the start distance again
                if (currentDistance >= startDistance) {
                    std::cout << "Sound is starting approach again..." << std::endl;
                    isApproaching = true;
                    currentDistance = startDistance;
                }
            }

            // Update emitter position
            approachEmitter->setPosition(0.0f, 0.0f, currentDistance);

            // Print current position every second
            static float timeSinceLastPrint = 0.0f;
            timeSinceLastPrint += deltaTime;

            if (timeSinceLastPrint >= 1.0f) {
                std::cout << "Sound distance: " << currentDistance << " - ";
                std::cout << (isApproaching ? "Approaching" : "Bouncing away") << std::endl;
                timeSinceLastPrint = 0.0f;
            }

            // Small delay between updates
            std::this_thread::sleep_for(std::chrono::milliseconds(16));  // ~60 fps

        }
        catch (const std::exception& e) {
            std::cerr << "Error during approach/bounce: " << e.what() << std::endl;
            running = false;
        }
    }

    // Clean up
    std::cout << "Cleaning up..." << std::endl;
    audioSystem.cleanup();
    std::cout << "Demo finished." << std::endl;

    return 0;
}