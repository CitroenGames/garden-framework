#version 450

// Vertex inputs - matches vertex struct layout
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoord;

// Outputs to fragment shader
layout(location = 0) out vec3 FragPos;
layout(location = 1) out vec3 Normal;
layout(location = 2) out vec2 TexCoord;
layout(location = 3) out float ViewDepth;  // For CSM cascade selection

// UBO for view/projection (per-frame data)
layout(set = 0, binding = 0) uniform GlobalUBO {
    mat4 view;
    mat4 projection;
    mat4 lightSpaceMatrices[4];
    vec4 cascadeSplits;
    vec3 lightDir;
    float cascadeSplit4;
    vec3 lightAmbient;
    int cascadeCount;
    vec3 lightDiffuse;
    int debugCascades;
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

    // Calculate view-space depth for cascade selection
    vec4 viewPos = ubo.view * worldPos;
    ViewDepth = -viewPos.z;  // Positive depth into screen

    gl_Position = ubo.projection * viewPos;
}
