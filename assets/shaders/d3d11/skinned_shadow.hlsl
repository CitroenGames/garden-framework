// Skinned shadow pass shader - depth only

cbuffer ShadowCB : register(b0)
{
    matrix uLightSpaceMatrix;
    matrix uModel;
};

static const int MAX_BONES = 128;

cbuffer BoneCB : register(b1)
{
    matrix uBones[MAX_BONES];
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

PSInput VSMain(VSInput input)
{
    PSInput output;

    float4 skinnedPos;

    if (uHasBones)
    {
        matrix boneTransform = uBones[input.boneIds[0]] * input.boneWeights[0]
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

// Empty pixel shader - only depth is written
void PSMain(PSInput input)
{
    // Depth-only pass - no color output needed
}
