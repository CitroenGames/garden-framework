#version 450

// Vertex input - position only needed for shadows
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;    // Not used but must match vertex format
layout(location = 2) in vec2 aTexCoord;  // Not used but must match vertex format

// UBO for light space matrix
layout(set = 0, binding = 0) uniform ShadowUBO {
    mat4 lightSpaceMatrix;
} ubo;

// Push constants for model matrix
layout(push_constant) uniform PushConstants {
    mat4 model;
} pc;

void main()
{
    gl_Position = ubo.lightSpaceMatrix * pc.model * vec4(aPos, 1.0);
}
