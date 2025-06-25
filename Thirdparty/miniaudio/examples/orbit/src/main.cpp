#include "AudioSystem.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <cmath>

int main() {
    std::cout << "Starting audio orbit demonstration..." << std::endl;
    
    // Create and initialize the audio system
    TE::AudioSystem audioSystem;
    
    if (!audioSystem.initialize()) {
        std::cerr << "Failed to initialize audio system." << std::endl;
        return -1;
    }
    
    std::cout << "Audio system initialized successfully." << std::endl;
    audioSystem.start();
    
    // Set up the audio listener (player)
    audioSystem.setListenerPosition(0.0f, 0.0f, 0.0f);      // Listener at center
    audioSystem.setListenerDirection(0.0f, 0.0f, -1.0f);    // Looking forward (negative Z)
    audioSystem.setListenerUp(0.0f, 1.0f, 0.0f);            // Up is positive Y
    
    // Configure sound parameters
    TE::SoundConfig config;
	config.type = TE::SoundType::Music;     // Music type for streaming (if not set correctly it wont work as expected)
    config.loop = true;                     // Make it loop continuously
    config.spatial = true;                  // Enable spatial audio
    config.minDistance = 1.0f;              // Sound is at full volume within this distance
    config.maxDistance = 20.0f;             // Sound fades to zero at this distance
    config.volume = 0.8f;                   // Initial volume
    
    // Create an emitter that will orbit around the listener
    auto orbiter = audioSystem.createEmitter("OrbitingSound");
    
    // Preload and play sound on the orbiter
    std::cout << "Preloading sound..." << std::endl;
    audioSystem.preloadSound("audio_sample.mp3", config);
    std::cout << "Playing orbiting sound..." << std::endl;
    auto soundInstance = audioSystem.playSound(orbiter, "audio_sample.mp3", config);
    
    if (!soundInstance) {
        std::cerr << "Failed to play sound!" << std::endl;
        audioSystem.cleanup();
        return -1;
    }
    
    // Orbit parameters
    float angle = 0.0f;                 // Current angle in radians
    float orbitRadius = 5.0f;           // Distance from listener (meters)
    float orbitHeight = 0.0f;           // Height relative to listener (meters)
    float orbitSpeed = 0.5f;            // Radians per second
    bool verticalOrbit = false;         // Whether to orbit vertically or horizontally
    
    std::cout << "Sound is now orbiting around the listener." << std::endl;
    std::cout << "Press Ctrl+C to exit." << std::endl;
    
    // Main orbit loop
    bool running = true;
    int orbitMode = 0;  // 0: horizontal, 1: vertical, 2: spiral
    
    while (running) {
        try {
            float x, y, z;
            
            // Update position based on orbit mode
            switch (orbitMode) {
                case 0:  // Horizontal orbit (around Y axis)
                    x = std::cos(angle) * orbitRadius;
                    y = orbitHeight;
                    z = std::sin(angle) * orbitRadius;
                    break;
                    
                case 1:  // Vertical orbit (around Z axis)
                    x = std::cos(angle) * orbitRadius;
                    y = std::sin(angle) * orbitRadius;
                    z = 0.0f;
                    break;
                    
                case 2:  // Spiral motion
                    x = std::cos(angle) * orbitRadius;
                    y = std::sin(angle * 0.5f) * 2.0f;  // Slower vertical oscillation
                    z = std::sin(angle) * orbitRadius;
                    break;
            }
            
            // Update emitter position
            orbiter->setPosition(x, y, z);
            
            // Print current position (every few seconds)
            if (fmod(angle, 1.0f) < 0.01f) {
                std::cout << "Sound position: (" << x << ", " << y << ", " << z << ")" << std::endl;
                
                // Change the orbit mode every full circle
                if (fmod(angle, 2 * M_PI) < 0.1f && angle > 0.1f) {
                    orbitMode = (orbitMode + 1) % 3;
                    std::cout << "Changing orbit mode to: ";
                    switch (orbitMode) {
                        case 0: std::cout << "Horizontal orbit" << std::endl; break;
                        case 1: std::cout << "Vertical orbit" << std::endl; break;
                        case 2: std::cout << "Spiral pattern" << std::endl; break;
                    }
                }
            }
            
            // Increase angle based on time and speed
            angle += orbitSpeed * 0.05f;  // Assuming ~50ms per frame
            
            // Small delay between updates
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            
        } catch (const std::exception& e) {
            std::cerr << "Error during orbit: " << e.what() << std::endl;
            running = false;
        }
    }
    
    // Clean up
    std::cout << "Cleaning up..." << std::endl;
    audioSystem.cleanup();
    std::cout << "Demo finished." << std::endl;
    
    return 0;
}