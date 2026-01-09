#pragma once

#include "Shader.hpp"
#include <string>
#include <unordered_map>

class ShaderManager
{
private:
    std::unordered_map<std::string, Shader*> shaders;

public:
    ShaderManager();
    ~ShaderManager();

    // Load a shader from files and cache it
    Shader* loadShader(const std::string& name, const std::string& vertex_path, const std::string& fragment_path);

    // Load a shader from source strings and cache it
    Shader* loadShaderFromStrings(const std::string& name, const std::string& vertex_src, const std::string& fragment_src);

    // Get a cached shader by name
    Shader* getShader(const std::string& name);

    // Create default shaders (basic, unlit)
    void createDefaultShaders();

    // Clear all shaders
    void clear();
};
