#version 450

layout(location = 0) in vec3 aPos;

layout(location = 0) out vec3 WorldPos;

layout(set = 0, binding = 0) uniform SkyboxUBO {
    mat4 view;
    mat4 projection;
    vec3 sunDirection;
    float time;
} ubo;

void main()
{
    WorldPos = aPos;
    // Remove translation from view matrix so the skybox stays centered around the camera
    mat4 view = mat4(mat3(ubo.view));
    vec4 pos = ubo.projection * view * vec4(aPos, 1.0);
    gl_Position = pos.xyww; // Force depth to maximum
}
