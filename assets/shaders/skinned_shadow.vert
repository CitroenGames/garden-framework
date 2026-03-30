#version 460 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoord;
layout(location = 3) in ivec4 aBoneIds;
layout(location = 4) in vec4 aBoneWeights;

uniform mat4 uLightSpaceMatrix;
uniform mat4 uModel;

const int MAX_BONES = 128;
uniform mat4 uBones[MAX_BONES];
uniform bool uHasBones;

void main()
{
    vec4 skinnedPos;

    if (uHasBones)
    {
        mat4 boneTransform = uBones[aBoneIds[0]] * aBoneWeights[0]
                           + uBones[aBoneIds[1]] * aBoneWeights[1]
                           + uBones[aBoneIds[2]] * aBoneWeights[2]
                           + uBones[aBoneIds[3]] * aBoneWeights[3];

        skinnedPos = boneTransform * vec4(aPos, 1.0);
    }
    else
    {
        skinnedPos = vec4(aPos, 1.0);
    }

    gl_Position = uLightSpaceMatrix * uModel * skinnedPos;
}
