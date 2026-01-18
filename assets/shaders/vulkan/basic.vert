#version 450

// Vertex inputs - matches vertex struct layout
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoord;

// Outputs to fragment shader
layout(location = 0) out vec3 FragPos;
layout(location = 1) out vec3 Normal;
layout(location = 2) out vec2 TexCoord;

// UBO for view/projection (per-frame data)
layout(set = 0, binding = 0) uniform GlobalUBO {
    mat4 view;
    mat4 projection;
    vec3 lightDir;
    float _pad1;
    vec3 lightAmbient;
    float _pad2;
    vec3 lightDiffuse;
    float _pad3;
    vec3 color;
    int useTexture;
} ubo;

// Push constants for model matrix (per-draw data)
layout(push_constant) uniform PushConstants {
    mat4 model;
} pc;

void main()
{
    vec4 worldPos = pc.model * vec4(aPos, 1.0);
    FragPos = worldPos.xyz;

    // Transform normal to world space
    Normal = mat3(transpose(inverse(pc.model))) * aNormal;

    TexCoord = aTexCoord;

    gl_Position = ubo.projection * ubo.view * worldPos;
}
