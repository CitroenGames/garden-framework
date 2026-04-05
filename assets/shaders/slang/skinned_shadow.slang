// Skinned shadow pass shader - depth only
import common;

[[vk::binding(0, 0)]]
cbuffer ShadowCB : register(b0)
{
    float4x4 uLightSpaceMatrix;
    float4x4 uModel;
};

[[vk::binding(1, 0)]]
cbuffer BoneCB : register(b1)
{
    float4x4 uBones[MAX_BONES];
    int uHasBones;
    float3 _bonePad;
};

struct VSInput
{
    float3 position    : POSITION;
    float3 normal      : NORMAL;
    float2 texcoord    : TEXCOORD0;
    int4   boneIds     : BLENDINDICES;
    float4 boneWeights : BLENDWEIGHT;
};

struct PSInput
{
    float4 position : SV_POSITION;
};

[shader("vertex")]
PSInput vertexMain(VSInput input)
{
    PSInput output;

    float4 skinnedPos;

    if (uHasBones != 0)
    {
        float4x4 boneTransform = uBones[input.boneIds[0]] * input.boneWeights[0]
                               + uBones[input.boneIds[1]] * input.boneWeights[1]
                               + uBones[input.boneIds[2]] * input.boneWeights[2]
                               + uBones[input.boneIds[3]] * input.boneWeights[3];

        skinnedPos = mul(boneTransform, float4(input.position, 1.0));
    }
    else
    {
        skinnedPos = float4(input.position, 1.0);
    }

    float4 worldPos = mul(uModel, skinnedPos);
    output.position = mul(uLightSpaceMatrix, worldPos);
    return output;
}

[shader("fragment")]
void fragmentMain(PSInput input)
{
    // Depth-only pass - no color output needed
}
