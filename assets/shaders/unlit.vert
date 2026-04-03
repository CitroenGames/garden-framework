#version 460 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoord;

out vec2 TexCoord;

layout(std140, binding = 0) uniform CameraData {
    mat4 uView;
    mat4 uProjection;
    vec4 uCameraPos;
};

uniform mat4 uModel;

void main()
{
    TexCoord = aTexCoord;
    gl_Position = uProjection * uView * uModel * vec4(aPos, 1.0);
}
