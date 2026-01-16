#pragma once

#include <glad/glad.h>
#include <string>
#include <unordered_map>
#include <glm/glm.hpp>

class Shader
{
private:
    GLuint program_id;
    std::unordered_map<std::string, GLint> uniform_cache;

    bool compileShader(const std::string& source, GLenum type, GLuint& shader_id);
    bool linkProgram(GLuint vertex_shader, GLuint fragment_shader);
    GLint getUniformLocation(const std::string& name);

public:
    Shader();
    ~Shader();

    // Load shader from source strings
    bool loadFromStrings(const std::string& vertex_src, const std::string& fragment_src);

    // Load shader from files
    bool loadFromFiles(const std::string& vertex_path, const std::string& fragment_path);

    // Use this shader for rendering
    void use() const;

    // Set uniform values
    void setUniform(const std::string& name, const glm::mat4& matrix);
    void setUniform(const std::string& name, const glm::vec3& vec);
    void setUniform(const std::string& name, const glm::vec2& vec);
    void setUniform(const std::string& name, float value);
    void setUniform(const std::string& name, int value);
    void setUniform(const std::string& name, bool value);

    // Get program ID
    GLuint getProgramID() const { return program_id; }

    // Check if shader is valid
    bool isValid() const { return program_id != 0; }
};
