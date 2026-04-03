#version 450

layout(push_constant) uniform PushConstants {
    mat4 transform;
    vec2 translation;
} pc;

layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec2 inTexCoord;

layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec2 fragTexCoord;

void main() {
    fragColor = inColor;
    fragTexCoord = inTexCoord;
    vec2 translated = inPosition + pc.translation;
    gl_Position = pc.transform * vec4(translated, 0.0, 1.0);
}
