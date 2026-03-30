#include <metal_stdlib>
using namespace metal;

struct ShadowUBO {
    float4x4 lightSpaceMatrix;
};

struct BoneUBO {
    float4x4 bones[128];
    int hasBones;
};

struct SkinnedShadowVertexIn {
    float3 position    [[attribute(0)]];
    float3 normal      [[attribute(1)]];
    float2 texCoord    [[attribute(2)]];
    int4   boneIds     [[attribute(3)]];
    float4 boneWeights [[attribute(4)]];
};

struct SkinnedShadowVertexOut {
    float4 position [[position]];
};

vertex SkinnedShadowVertexOut skinned_shadow_vertex(SkinnedShadowVertexIn in [[stage_in]],
                                                      constant ShadowUBO& ubo [[buffer(1)]],
                                                      constant float4x4& model [[buffer(2)]],
                                                      constant BoneUBO& boneUbo [[buffer(3)]])
{
    SkinnedShadowVertexOut out;

    float4 skinnedPos;

    if (boneUbo.hasBones != 0)
    {
        float4x4 boneTransform = boneUbo.bones[in.boneIds[0]] * in.boneWeights[0]
                               + boneUbo.bones[in.boneIds[1]] * in.boneWeights[1]
                               + boneUbo.bones[in.boneIds[2]] * in.boneWeights[2]
                               + boneUbo.bones[in.boneIds[3]] * in.boneWeights[3];

        skinnedPos = boneTransform * float4(in.position, 1.0);
    }
    else
    {
        skinnedPos = float4(in.position, 1.0);
    }

    out.position = ubo.lightSpaceMatrix * model * skinnedPos;
    return out;
}

fragment void skinned_shadow_fragment()
{
    // Depth is written automatically
}
