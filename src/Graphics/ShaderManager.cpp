#include "ShaderManager.hpp"
#include <iostream>


ShaderManager::ShaderManager()
{
}

ShaderManager::~ShaderManager()
{
    clear();
}

Shader* ShaderManager::loadShader(const std::string& name, const std::string& vertex_path, const std::string& fragment_path)
{
    // Check if shader already exists
    auto it = shaders.find(name);
    if (it != shaders.end())
    {
        printf("Shader '%s' already loaded\n", name.c_str());
        return it->second;
    }

    // Create and load new shader
    Shader* shader = new Shader();
    if (!shader->loadFromFiles(vertex_path, fragment_path))
    {
        delete shader;
        printf("Failed to load shader '%s'\n", name.c_str());
        return nullptr;
    }

    // Cache the shader
    shaders[name] = shader;
    printf("Shader '%s' loaded and cached\n", name.c_str());
    return shader;
}

Shader* ShaderManager::loadShaderFromStrings(const std::string& name, const std::string& vertex_src, const std::string& fragment_src)
{
    // Check if shader already exists
    auto it = shaders.find(name);
    if (it != shaders.end())
    {
        printf("Shader '%s' already loaded\n", name.c_str());
        return it->second;
    }

    // Create and load new shader
    Shader* shader = new Shader();
    if (!shader->loadFromStrings(vertex_src, fragment_src))
    {
        delete shader;
        printf("Failed to load shader '%s' from strings\n", name.c_str());
        return nullptr;
    }

    // Cache the shader
    shaders[name] = shader;
    printf("Shader '%s' loaded and cached from strings\n", name.c_str());
    return shader;
}

Shader* ShaderManager::getShader(const std::string& name)
{
    auto it = shaders.find(name);
    if (it != shaders.end())
    {
        return it->second;
    }

    printf("Warning: Shader '%s' not found\n", name.c_str());
    return nullptr;
}

void ShaderManager::createDefaultShaders()
{
    printf("Creating default shaders from files...\n");

    std::string shader_dir = "assets/shaders/";

    // Load basic shader (with lighting)
    if (!loadShader("basic", shader_dir + "basic.vert", shader_dir + "basic.frag")) {
        printf("ERROR: Failed to load basic shader from %s\n", shader_dir.c_str());
    }

    // Load unlit shader (without lighting)
    if (!loadShader("unlit", shader_dir + "unlit.vert", shader_dir + "unlit.frag")) {
        printf("ERROR: Failed to load unlit shader from %s\n", shader_dir.c_str());
    }

    printf("Default shaders loaded\n");
}

void ShaderManager::clear()
{
    for (auto& pair : shaders)
    {
        delete pair.second;
    }
    shaders.clear();
}
