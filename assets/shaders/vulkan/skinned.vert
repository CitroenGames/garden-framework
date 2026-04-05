#version 450

// Vertex inputs - matches skinned_vertex struct layout
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoord;
layout(location = 3) in ivec4 aBoneIds;
layout(location = 4) in vec4 aBoneWeights;

// Outputs to fragment shader
layout(location = 0) out vec3 FragPos;
layout(location = 1) out vec3 Normal;
layout(location = 2) out vec2 TexCoord;
layout(location = 3) out float ViewDepth;

struct PointLightData {
    vec3 position; float range;
    vec3 color; float intensity;
    vec3 attenuation; float _pad;
};

struct SpotLightData {
    vec3 position; float range;
    vec3 direction; float intensity;
    vec3 color; float innerCutoff;
    vec3 attenuation; float outerCutoff;
};

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
    PointLightData pointLights[16];
    SpotLightData  spotLights[16];
    int numPointLights;
    int numSpotLights;
    vec2 _lightPad;
    vec3 cameraPos;
    float _lightPad2;
} ubo;

// Push constants for model matrix (per-draw data)
layout(push_constant) uniform PushConstants {
    mat4 model;
} pc;

// Bone matrices UBO
const int MAX_BONES = 128;
layout(set = 0, binding = 3) uniform BoneUBO {
    mat4 bones[MAX_BONES];
    int hasBones;
} boneUbo;

void main()
{
    vec4 skinnedPos;
    vec3 skinnedNormal;

    if (boneUbo.hasBones != 0)
    {
        mat4 boneTransform = boneUbo.bones[aBoneIds[0]] * aBoneWeights[0]
                           + boneUbo.bones[aBoneIds[1]] * aBoneWeights[1]
                           + boneUbo.bones[aBoneIds[2]] * aBoneWeights[2]
                           + boneUbo.bones[aBoneIds[3]] * aBoneWeights[3];

        skinnedPos = boneTransform * vec4(aPos, 1.0);
        skinnedNormal = mat3(boneTransform) * aNormal;
    }
    else
    {
        skinnedPos = vec4(aPos, 1.0);
        skinnedNormal = aNormal;
    }

    vec4 worldPos = pc.model * skinnedPos;
    FragPos = worldPos.xyz;

    Normal = mat3(transpose(inverse(pc.model))) * skinnedNormal;

    TexCoord = aTexCoord;

    vec4 viewPos = ubo.view * worldPos;
    ViewDepth = -viewPos.z;

    gl_Position = ubo.projection * viewPos;
}
