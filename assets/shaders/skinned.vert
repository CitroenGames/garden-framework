#version 460 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoord;
layout(location = 3) in ivec4 aBoneIds;
layout(location = 4) in vec4 aBoneWeights;

out vec3 FragPos;
out vec3 Normal;
out vec2 TexCoord;
out float ViewDepth;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProjection;

const int MAX_BONES = 128;
uniform mat4 uBones[MAX_BONES];
uniform bool uHasBones;

void main()
{
    vec4 skinnedPos;
    vec3 skinnedNormal;

    if (uHasBones)
    {
        mat4 boneTransform = uBones[aBoneIds[0]] * aBoneWeights[0]
                           + uBones[aBoneIds[1]] * aBoneWeights[1]
                           + uBones[aBoneIds[2]] * aBoneWeights[2]
                           + uBones[aBoneIds[3]] * aBoneWeights[3];

        skinnedPos = boneTransform * vec4(aPos, 1.0);
        skinnedNormal = mat3(boneTransform) * aNormal;
    }
    else
    {
        skinnedPos = vec4(aPos, 1.0);
        skinnedNormal = aNormal;
    }

    vec4 worldPos = uModel * skinnedPos;
    FragPos = worldPos.xyz;
    Normal = mat3(transpose(inverse(uModel))) * skinnedNormal;
    TexCoord = aTexCoord;

    vec4 viewPos = uView * worldPos;
    ViewDepth = -viewPos.z;

    gl_Position = uProjection * viewPos;
}
