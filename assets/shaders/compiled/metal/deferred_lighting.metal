#include <metal_stdlib>
using namespace metal;

struct DeferredLightingUniforms {
    float4x4 invViewProj;
    float4x4 view;
    float4x4 lightSpaceMatrices[4];
    float4 cascadeSplits;
    float cascadeSplit4;
    int cascadeCount;
    float2 shadowMapTexelSize;
    packed_float3 cameraPos;
    float _pad0;
    packed_float3 lightDir;
    float _pad1;
    packed_float3 lightAmbient;
    float _pad2;
    packed_float3 lightDiffuse;
    float _pad3;
    int numPointLights;
    int numSpotLights;
    float2 _pad4;
};

struct DeferredVertexIn {
    float2 position [[attribute(0)]];
    float2 texCoord [[attribute(1)]];
};

struct DeferredVertexOut {
    float4 position [[position]];
    float2 texCoord;
};

vertex DeferredVertexOut deferred_lighting_vertex(DeferredVertexIn in [[stage_in]])
{
    DeferredVertexOut out;
    out.position = float4(in.position, 0.0, 1.0);
    out.texCoord = in.texCoord;
    return out;
}

float deferredCascadeSplit(constant DeferredLightingUniforms& u, int index)
{
    if (index == 0) return u.cascadeSplits.x;
    if (index == 1) return u.cascadeSplits.y;
    if (index == 2) return u.cascadeSplits.z;
    if (index == 3) return u.cascadeSplits.w;
    return u.cascadeSplit4;
}

int deferredCascadeIndex(constant DeferredLightingUniforms& u, float viewDepth)
{
    if (u.cascadeCount <= 0) return 0;
    for (int i = 0; i < u.cascadeCount; ++i) {
        if (viewDepth < deferredCascadeSplit(u, i + 1))
            return i;
    }
    return max(u.cascadeCount - 1, 0);
}

float deferredShadowSingle(constant DeferredLightingUniforms& u,
                           int cascadeIndex,
                           float3 fragPos,
                           float3 normal,
                           depth2d_array<float> shadowMap,
                           sampler shadowSampler)
{
    float4 lsPos = u.lightSpaceMatrices[cascadeIndex] * float4(fragPos, 1.0);
    float3 proj = lsPos.xyz / lsPos.w;
    proj.xy = proj.xy * 0.5 + 0.5;
    proj.y = 1.0 - proj.y;

    if (proj.z > 1.0 || proj.z < 0.0)
        return 1.0;

    float3 L = normalize(-float3(u.lightDir));
    float bias = max(0.0005 * (1.0 - dot(normalize(normal), L)), 0.00005);
    bias *= (1.0 + float(cascadeIndex) * 0.5);

    float shadow = 0.0;
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            float2 uv = proj.xy + float2(x, y) * u.shadowMapTexelSize;
            shadow += shadowMap.sample_compare(shadowSampler, uv, cascadeIndex, proj.z - bias);
        }
    }
    return shadow / 9.0;
}

float deferredShadow(constant DeferredLightingUniforms& u,
                     float3 fragPos,
                     float3 normal,
                     float viewDepth,
                     depth2d_array<float> shadowMap,
                     sampler shadowSampler)
{
    if (u.cascadeCount <= 0)
        return 1.0;

    int cascadeIndex = deferredCascadeIndex(u, viewDepth);
    float shadow = deferredShadowSingle(u, cascadeIndex, fragPos, normal, shadowMap, shadowSampler);

    if (cascadeIndex < u.cascadeCount - 1) {
        float cascadeEnd = deferredCascadeSplit(u, cascadeIndex + 1);
        float cascadeStart = deferredCascadeSplit(u, cascadeIndex);
        float cascadeRange = cascadeEnd - cascadeStart;
        float distToEnd = cascadeEnd - viewDepth;
        if (distToEnd < cascadeRange * 0.1) {
            float blendFactor = distToEnd / max(cascadeRange * 0.1, 0.0001);
            float nextShadow = deferredShadowSingle(u, cascadeIndex + 1, fragPos, normal, shadowMap, shadowSampler);
            shadow = mix(nextShadow, shadow, blendFactor);
        }
    }
    return shadow;
}

fragment float4 deferred_lighting_fragment(DeferredVertexOut in [[stage_in]],
                                           constant DeferredLightingUniforms& u [[buffer(0)]],
                                           const device PointLightData* pointLights [[buffer(1)]],
                                           const device SpotLightData* spotLights [[buffer(2)]],
                                           texture2d<float> gb0 [[texture(0)]],
                                           texture2d<float> gb1 [[texture(1)]],
                                           texture2d<float> gb2 [[texture(2)]],
                                           depth2d<float> sceneDepth [[texture(3)]],
                                           depth2d_array<float> shadowMap [[texture(4)]],
                                           sampler linearSampler [[sampler(0)]],
                                           sampler depthSampler [[sampler(1)]],
                                           sampler shadowSampler [[sampler(2)]])
{
    float2 uv = in.texCoord;
    float depth = sceneDepth.sample(depthSampler, uv);
    if (depth >= 0.9999)
        discard_fragment();

    float4 g0 = gb0.sample(linearSampler, uv);
    float4 g1 = gb1.sample(linearSampler, uv);
    float4 g2 = gb2.sample(linearSampler, uv);

    float2 ndc = float2(uv.x * 2.0 - 1.0, (1.0 - uv.y) * 2.0 - 1.0);
    float4 clipPos = float4(ndc, depth, 1.0);
    float4 worldPosH = u.invViewProj * clipPos;
    float3 fragPos = worldPosH.xyz / worldPosH.w;

    float3 albedo = g0.rgb;
    float metallic = g0.a;
    float3 N = normalize(g1.rgb);
    float roughness = max(g1.a, 0.04);
    float3 emissive = g2.rgb;
    float ao = g2.a;

    float3 V = normalize(float3(u.cameraPos) - fragPos);
    float3 L = normalize(-float3(u.lightDir));
    float viewDepth = -(u.view * float4(fragPos, 1.0)).z;
    float shadow = deferredShadow(u, fragPos, N, viewDepth, shadowMap, shadowSampler);

    float3 lighting = float3(u.lightAmbient) * albedo * ao;
    lighting += shadow * CalcPBRDirectional(N, V, L, float3(u.lightDiffuse), albedo, metallic, roughness);
    lighting += emissive;

    for (int i = 0; i < u.numPointLights; ++i)
        lighting += CalcPBRPointLight(pointLights[i], N, V, fragPos, albedo, metallic, roughness);

    for (int i = 0; i < u.numSpotLights; ++i)
        lighting += CalcPBRSpotLight(spotLights[i], N, V, fragPos, albedo, metallic, roughness);

    return float4(lighting, 1.0);
}
