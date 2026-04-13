#include <metal_stdlib>
using namespace metal;

// Shared types and functions are in common.metal

struct SkinnedVertexIn {
    float3 position    [[attribute(0)]];
    float3 normal      [[attribute(1)]];
    float2 texCoord    [[attribute(2)]];
    int4   boneIds     [[attribute(3)]];
    float4 boneWeights [[attribute(4)]];
};

struct SkinnedVertexOut {
    float4 position [[position]];
    float3 fragPos;
    float3 normal;
    float2 texCoord;
    float viewDepth;
};

vertex SkinnedVertexOut skinned_vertex(SkinnedVertexIn in [[stage_in]],
                                        constant GlobalUBO& ubo [[buffer(1)]],
                                        constant ModelData& modelData [[buffer(2)]],
                                        constant BoneUBO& boneUbo [[buffer(3)]])
{
    SkinnedVertexOut out;

    float4 skinnedPos;
    float3 skinnedNormal;

    if (boneUbo.hasBones != 0)
    {
        float4x4 boneTransform = boneUbo.bones[in.boneIds[0]] * in.boneWeights[0]
                               + boneUbo.bones[in.boneIds[1]] * in.boneWeights[1]
                               + boneUbo.bones[in.boneIds[2]] * in.boneWeights[2]
                               + boneUbo.bones[in.boneIds[3]] * in.boneWeights[3];

        skinnedPos = boneTransform * float4(in.position, 1.0);
        skinnedNormal = float3x3(boneTransform[0].xyz, boneTransform[1].xyz, boneTransform[2].xyz) * in.normal;
    }
    else
    {
        skinnedPos = float4(in.position, 1.0);
        skinnedNormal = in.normal;
    }

    float4 worldPos = modelData.model * skinnedPos;
    out.fragPos = worldPos.xyz;

    // Transform normal using pre-computed normal matrix (inverse-transpose)
    float3x3 normalMatrix = float3x3(modelData.normalMatrix[0].xyz,
                                      modelData.normalMatrix[1].xyz,
                                      modelData.normalMatrix[2].xyz);
    out.normal = normalize(normalMatrix * skinnedNormal);

    out.texCoord = in.texCoord;

    // Calculate view-space depth for cascade selection
    float4 viewPos = ubo.view * worldPos;
    out.viewDepth = -viewPos.z;

    out.position = ubo.projection * viewPos;

    return out;
}

fragment float4 skinned_fragment(SkinnedVertexOut in [[stage_in]],
                                  constant GlobalUBO& ubo [[buffer(0)]],
                                  texture2d<float> tex [[texture(0)]],
                                  sampler texSampler [[sampler(0)]],
                                  depth2d_array<float> shadowMap [[texture(1)]],
                                  sampler shadowSampler [[sampler(1)]],
                                  constant LightCBuffer& lights [[buffer(3)]])
{
    float3 norm = normalize(in.normal);
    float3 lightDir = normalize(-ubo.lightDir);
    float diff = max(dot(norm, lightDir), 0.0);

    float3 ambient = ubo.lightAmbient;
    float3 diffuse = ubo.lightDiffuse * diff;

    // Calculate shadow using CSM
    float shadow = 0.0;
    if (ubo.cascadeCount > 0) {
        shadow = ShadowCalculationWithBlend(ubo, in.fragPos, norm, in.viewDepth, shadowMap, shadowSampler);
    }
    float3 lighting = ambient + shadow * diffuse;

    // Point lights
    for (int i = 0; i < lights.numPointLights; i++)
        lighting += CalcPointLight(lights.pointLights[i], norm, in.fragPos);

    // Spot lights
    for (int j = 0; j < lights.numSpotLights; j++)
        lighting += CalcSpotLight(lights.spotLights[j], norm, in.fragPos);

    float4 texColor;
    if (ubo.useTexture != 0) {
        texColor = tex.sample(texSampler, in.texCoord);
        if (ubo.alphaCutoff > 0.0 && texColor.a < ubo.alphaCutoff)
            discard_fragment();
    } else {
        texColor = float4(ubo.color, 1.0);
    }

    return float4(lighting * texColor.rgb, texColor.a);
}
