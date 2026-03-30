#version 450

// Vertex inputs - matches skinned_vertex struct layout
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoord;
layout(location = 3) in ivec4 aBoneIds;
layout(location = 4) in vec4 aBoneWeights;

// UBO for light space matrix
layout(set = 0, binding = 0) uniform ShadowUBO {
    mat4 lightSpaceMatrix;
} ubo;

// Push constants for model matrix
layout(push_constant) uniform PushConstants {
    mat4 model;
} pc;

// Bone matrices UBO
const int MAX_BONES = 128;
layout(set = 0, binding = 1) uniform BoneUBO {
    mat4 bones[MAX_BONES];
    int hasBones;
} boneUbo;

void main()
{
    vec4 skinnedPos;

    if (boneUbo.hasBones != 0)
    {
        mat4 boneTransform = boneUbo.bones[aBoneIds[0]] * aBoneWeights[0]
                           + boneUbo.bones[aBoneIds[1]] * aBoneWeights[1]
                           + boneUbo.bones[aBoneIds[2]] * aBoneWeights[2]
                           + boneUbo.bones[aBoneIds[3]] * aBoneWeights[3];

        skinnedPos = boneTransform * vec4(aPos, 1.0);
    }
    else
    {
        skinnedPos = vec4(aPos, 1.0);
    }

    gl_Position = ubo.lightSpaceMatrix * pc.model * skinnedPos;
}
