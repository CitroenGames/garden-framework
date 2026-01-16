#include "Shader.hpp"
#include <fstream>
#include <sstream>
#include <iostream>
#include <glm/gtc/type_ptr.hpp>

Shader::Shader()
    : program_id(0)
{
}

Shader::~Shader()
{
    if (program_id != 0)
    {
        glDeleteProgram(program_id);
        program_id = 0;
    }
}

bool Shader::compileShader(const std::string& source, GLenum type, GLuint& shader_id)
{
    shader_id = glCreateShader(type);
    const char* source_cstr = source.c_str();
    glShaderSource(shader_id, 1, &source_cstr, nullptr);
    glCompileShader(shader_id);

    // Check compilation status
    GLint success;
    glGetShaderiv(shader_id, GL_COMPILE_STATUS, &success);
    if (!success)
    {
        GLchar info_log[1024];
        glGetShaderInfoLog(shader_id, 1024, nullptr, info_log);
        const char* type_str = (type == GL_VERTEX_SHADER) ? "VERTEX" : "FRAGMENT";
        printf("Shader compilation error (%s):\n%s\n", type_str, info_log);
        glDeleteShader(shader_id);
        shader_id = 0;
        return false;
    }

    return true;
}

bool Shader::linkProgram(GLuint vertex_shader, GLuint fragment_shader)
{
    program_id = glCreateProgram();
    glAttachShader(program_id, vertex_shader);
    glAttachShader(program_id, fragment_shader);
    glLinkProgram(program_id);

    // Check linking status
    GLint success;
    glGetProgramiv(program_id, GL_LINK_STATUS, &success);
    if (!success)
    {
        GLchar info_log[1024];
        glGetProgramInfoLog(program_id, 1024, nullptr, info_log);
        printf("Shader linking error:\n%s\n", info_log);
        glDeleteProgram(program_id);
        program_id = 0;
        return false;
    }

    // Detach shaders after linking (they're no longer needed)
    glDetachShader(program_id, vertex_shader);
    glDetachShader(program_id, fragment_shader);

    return true;
}

bool Shader::loadFromStrings(const std::string& vertex_src, const std::string& fragment_src)
{
    // Clean up existing program if any
    if (program_id != 0)
    {
        glDeleteProgram(program_id);
        program_id = 0;
        uniform_cache.clear();
    }

    // Compile vertex shader
    GLuint vertex_shader = 0;
    if (!compileShader(vertex_src, GL_VERTEX_SHADER, vertex_shader))
    {
        return false;
    }

    // Compile fragment shader
    GLuint fragment_shader = 0;
    if (!compileShader(fragment_src, GL_FRAGMENT_SHADER, fragment_shader))
    {
        glDeleteShader(vertex_shader);
        return false;
    }

    // Link program
    if (!linkProgram(vertex_shader, fragment_shader))
    {
        glDeleteShader(vertex_shader);
        glDeleteShader(fragment_shader);
        return false;
    }

    // Clean up shaders (program has them now)
    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);

    printf("Shader program compiled and linked successfully (ID: %u)\n", program_id);
    return true;
}

bool Shader::loadFromFiles(const std::string& vertex_path, const std::string& fragment_path)
{
    // Read vertex shader
    std::ifstream vertex_file(vertex_path);
    if (!vertex_file.is_open())
    {
        printf("Failed to open vertex shader file: %s\n", vertex_path.c_str());
        return false;
    }
    std::stringstream vertex_stream;
    vertex_stream << vertex_file.rdbuf();
    std::string vertex_src = vertex_stream.str();
    vertex_file.close();

    // Read fragment shader
    std::ifstream fragment_file(fragment_path);
    if (!fragment_file.is_open())
    {
        printf("Failed to open fragment shader file: %s\n", fragment_path.c_str());
        return false;
    }
    std::stringstream fragment_stream;
    fragment_stream << fragment_file.rdbuf();
    std::string fragment_src = fragment_stream.str();
    fragment_file.close();

    printf("Loading shaders: %s, %s\n", vertex_path.c_str(), fragment_path.c_str());
    return loadFromStrings(vertex_src, fragment_src);
}

void Shader::use() const
{
    if (program_id != 0)
    {
        glUseProgram(program_id);
    }
}

GLint Shader::getUniformLocation(const std::string& name)
{
    // Check cache first
    auto it = uniform_cache.find(name);
    if (it != uniform_cache.end())
    {
        return it->second;
    }

    // Query OpenGL for location
    GLint location = glGetUniformLocation(program_id, name.c_str());
    if (location == -1)
    {
        // Uniform not found - this is not necessarily an error (might be optimized out)
        // printf("Warning: Uniform '%s' not found in shader\n", name.c_str());
    }

    // Cache the location (even if it's -1)
    uniform_cache[name] = location;
    return location;
}

void Shader::setUniform(const std::string& name, const glm::mat4& matrix)
{
    GLint location = getUniformLocation(name);
    if (location != -1)
    {
        glUniformMatrix4fv(location, 1, GL_FALSE, glm::value_ptr(matrix));
    }
}

void Shader::setUniform(const std::string& name, const glm::vec3& vec)
{
    GLint location = getUniformLocation(name);
    if (location != -1)
    {
        glUniform3fv(location, 1, glm::value_ptr(vec));
    }
}

void Shader::setUniform(const std::string& name, const glm::vec2& vec)
{
    GLint location = getUniformLocation(name);
    if (location != -1)
    {
        glUniform2fv(location, 1, glm::value_ptr(vec));
    }
}

void Shader::setUniform(const std::string& name, float value)
{
    GLint location = getUniformLocation(name);
    if (location != -1)
    {
        glUniform1f(location, value);
    }
}

void Shader::setUniform(const std::string& name, int value)
{
    GLint location = getUniformLocation(name);
    if (location != -1)
    {
        glUniform1i(location, value);
    }
}

void Shader::setUniform(const std::string& name, bool value)
{
    setUniform(name, value ? 1 : 0);
}
