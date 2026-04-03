#version 460 core
layout (location = 0) in vec3 aPos;

out vec3 WorldPos;

layout(std140, binding = 0) uniform CameraData {
    mat4 uView;
    mat4 uProjection;
    vec4 uCameraPos;
};

void main()
{
    WorldPos = aPos;
    // Remove translation from view matrix so the skybox stays centered around the camera
    mat4 view = mat4(mat3(uView));
    vec4 pos = uProjection * view * vec4(aPos, 1.0);
    gl_Position = pos.xyww; // Force depth to maximum
}
