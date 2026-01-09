#include "ShaderManager.hpp"
#include <iostream>

// Embedded default shader sources
namespace DefaultShaders
{
    const char* basic_vertex = R"(
#version 460 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoord;

out vec3 FragPos;
out vec3 Normal;
out vec2 TexCoord;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProjection;

void main()
{
    FragPos = vec3(uModel * vec4(aPos, 1.0));
    Normal = mat3(transpose(inverse(uModel))) * aNormal;
    TexCoord = aTexCoord;
    gl_Position = uProjection * uView * vec4(FragPos, 1.0);
}
)";

    const char* basic_fragment = R"(
#version 460 core

in vec3 FragPos;
in vec3 Normal;
in vec2 TexCoord;

out vec4 FragColor;

uniform sampler2D uTexture;
uniform bool uUseTexture;
uniform vec3 uLightPos;
uniform vec3 uLightAmbient;
uniform vec3 uLightDiffuse;
uniform vec3 uColor;

void main()
{
    vec3 norm = normalize(Normal);
    vec3 lightDir = normalize(uLightPos - FragPos);
    float diff = max(dot(norm, lightDir), 0.0);

    vec3 ambient = uLightAmbient;
    vec3 diffuse = uLightDiffuse * diff;
    vec3 lighting = ambient + diffuse;

    vec4 texColor = uUseTexture ? texture(uTexture, TexCoord) : vec4(uColor, 1.0);
    FragColor = vec4(lighting * texColor.rgb, texColor.a);
}
)";

    const char* unlit_vertex = R"(
#version 460 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoord;

out vec2 TexCoord;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProjection;

void main()
{
    TexCoord = aTexCoord;
    gl_Position = uProjection * uView * uModel * vec4(aPos, 1.0);
}
)";

    const char* unlit_fragment = R"(
#version 460 core

in vec2 TexCoord;

out vec4 FragColor;

uniform sampler2D uTexture;
uniform bool uUseTexture;
uniform vec3 uColor;

void main()
{
    if (uUseTexture)
    {
        FragColor = texture(uTexture, TexCoord);
    }
    else
    {
        FragColor = vec4(uColor, 1.0);
    }
}
)";
}

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
    printf("Creating default shaders...\n");

    // Load basic shader (with lighting)
    loadShaderFromStrings("basic", DefaultShaders::basic_vertex, DefaultShaders::basic_fragment);

    // Load unlit shader (without lighting)
    loadShaderFromStrings("unlit", DefaultShaders::unlit_vertex, DefaultShaders::unlit_fragment);

    printf("Default shaders created\n");
}

void ShaderManager::clear()
{
    for (auto& pair : shaders)
    {
        delete pair.second;
    }
    shaders.clear();
}
