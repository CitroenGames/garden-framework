// Deferred lighting pass. Samples the GBuffer, reconstructs world position
// from depth, and applies ambient + emissive + PBR directional light with
// CSM shadows.
// GBuffer layout (matches gbuffer.slang):
//   gb0 = BaseColor(rgb) + Metallic(a)
//   gb1 = WorldNormal(rgb) + Roughness(a)
//   gb2 = Emissive(rgb)   + AO(a)
import common;
import pbr;

[[vk::binding(0, 0)]]
cbuffer DeferredLightingCB : register(b0)
{
    float4x4 uInvViewProj;
    float4x4 uView;
    float4x4 uLightSpaceMatrices[4];
    float4   uCascadeSplits;
    float    uCascadeSplit4;
    int      uCascadeCount;
    float2   uShadowMapTexelSize;
    float3   uCameraPos;
    float    _pad0;
    float3   uLightDir;
    float    _pad1;
    float3   uLightAmbient;
    float    _pad2;
    float3   uLightDiffuse;
    float    _pad3;
    int      uNumPointLights;
    int      uNumSpotLights;
    float2   _pad4;
};

[[vk::binding(1, 0)]]Texture2D gb0 : register(t0);
[[vk::binding(1, 0)]]SamplerState gb0Sampler : register(s0);

[[vk::binding(2, 0)]]Texture2D gb1 : register(t1);
[[vk::binding(2, 0)]]SamplerState gb1Sampler : register(s1);

[[vk::binding(3, 0)]]Texture2D gb2 : register(t2);
[[vk::binding(3, 0)]]SamplerState gb2Sampler : register(s2);

[[vk::binding(4, 0)]]Texture2D sceneDepth : register(t3);
[[vk::binding(4, 0)]]SamplerState depthSampler : register(s3);

[[vk::binding(5, 0)]]Texture2DArray shadowMapArray : register(t4);
[[vk::binding(5, 0)]]SamplerComparisonState shadowSampler : register(s4);

[[vk::binding(6, 0)]]StructuredBuffer<PointLightData> pointLights : register(t5);
[[vk::binding(7, 0)]]StructuredBuffer<SpotLightData>  spotLights  : register(t6);

struct VSInput { float2 position : POSITION; float2 texcoord : TEXCOORD0; };
struct PSInput { float4 position : SV_POSITION; float2 texcoord : TEXCOORD0; };

[shader("vertex")]
PSInput vertexMain(VSInput input)
{
    PSInput o;
    o.position = float4(input.position, 0.0, 1.0);
#ifdef TARGET_HLSL
    o.texcoord = float2(input.texcoord.x, 1.0 - input.texcoord.y);
#else
    o.texcoord = input.texcoord;
#endif
    return o;
}

int getCascadeIndex(float viewDepth)
{
    float splits[5];
    splits[0] = uCascadeSplits.x;
    splits[1] = uCascadeSplits.y;
    splits[2] = uCascadeSplits.z;
    splits[3] = uCascadeSplits.w;
    splits[4] = uCascadeSplit4;

    for (int i = 0; i < uCascadeCount; i++)
    {
        if (viewDepth < splits[i + 1])
            return i;
    }
    return max(uCascadeCount - 1, 0);
}

float ShadowSampleSingle(int cascadeIndex, float3 fragPos, float3 N, float2 screenPos)
{
    float4 lsPos = mul(uLightSpaceMatrices[cascadeIndex], float4(fragPos, 1.0));
    float3 proj = lsPos.xyz / lsPos.w;
    proj.xy = proj.xy * 0.5 + 0.5;
#ifdef TARGET_HLSL
    proj.y = 1.0 - proj.y;
#endif
    float currentDepth = proj.z;
    if (currentDepth > 1.0 || currentDepth < 0.0) return 1.0;

    float3 L = normalize(-uLightDir);
    float baseBias = max(0.0005 * (1.0 - dot(normalize(N), L)), 0.00005);
    float bias = baseBias * (1.0 + float(cascadeIndex) * 0.5);

    float angle = frac(sin(dot(screenPos, float2(12.9898, 78.233))) * 43758.5453) * 6.28318530718;
    float cosA = cos(angle);
    float sinA = sin(angle);

    float shadow = 0.0;
    float2 texel = uShadowMapTexelSize;
    float spread = 1.5;

    [unroll]
    for (int i = 0; i < POISSON_SAMPLE_COUNT; i++)
    {
        float2 o = poissonDisk[i];
        float2 r = float2(o.x * cosA - o.y * sinA, o.x * sinA + o.y * cosA);
        float2 uv = proj.xy + r * texel * spread;
        shadow += shadowMapArray.SampleCmpLevelZero(shadowSampler, float3(uv, cascadeIndex), currentDepth - bias);
    }
    return shadow / float(POISSON_SAMPLE_COUNT);
}

float ShadowSample(int cascadeIndex, float3 fragPos, float3 N, float viewDepth, float2 screenPos)
{
    float shadow = ShadowSampleSingle(cascadeIndex, fragPos, N, screenPos);

    if (cascadeIndex < uCascadeCount - 1)
    {
        float splits[5];
        splits[0] = uCascadeSplits.x;
        splits[1] = uCascadeSplits.y;
        splits[2] = uCascadeSplits.z;
        splits[3] = uCascadeSplits.w;
        splits[4] = uCascadeSplit4;

        float cascadeEnd = splits[cascadeIndex + 1];
        float blendRange = cascadeEnd * 0.1;
        float blendStart = cascadeEnd - blendRange;

        if (viewDepth > blendStart)
        {
            float blendFactor = smoothstep(blendStart, cascadeEnd, viewDepth);
            float nextShadow = ShadowSampleSingle(cascadeIndex + 1, fragPos, N, screenPos);
            shadow = lerp(shadow, nextShadow, blendFactor);
        }
    }
    return shadow;
}

[shader("fragment")]
float4 fragmentMain(PSInput input) : SV_Target
{
    float4 g0 = gb0.Sample(gb0Sampler, input.texcoord);
    float4 g1 = gb1.Sample(gb1Sampler, input.texcoord);
    float4 g2 = gb2.Sample(gb2Sampler, input.texcoord);
    float  d  = sceneDepth.Sample(depthSampler, input.texcoord).r;

    // Sky pixels (far plane) — let skybox pass fill them.
    if (d >= 1.0) discard;

    float3 albedo    = g0.rgb;
    float  metallic  = g0.a;
    float3 N         = normalize(g1.rgb);
    float  roughness = max(g1.a, 0.04);
    float3 emissive  = g2.rgb;
    float  ao        = g2.a;

    // Reconstruct world position from depth.
    float2 ndc = input.texcoord * 2.0 - 1.0;
#ifdef TARGET_HLSL
    ndc.y = -ndc.y;
#endif
    float4 clipPos   = float4(ndc, d, 1.0);
    float4 worldPosH = mul(uInvViewProj, clipPos);
    float3 fragPos   = worldPosH.xyz / worldPosH.w;

    float3 V = normalize(uCameraPos - fragPos);
    float3 L = normalize(-uLightDir);

    // View-space depth for cascade selection.
    float viewDepth  = -mul(uView, float4(fragPos, 1.0)).z;
    int   cascadeIdx = getCascadeIndex(viewDepth);
    float shadow     = ShadowSample(cascadeIdx, fragPos, N, viewDepth, input.position.xy);

    float3 ambient = uLightAmbient * albedo * ao;
    float3 direct  = shadow * CalcPBRDirectional(N, V, L, uLightDiffuse,
                                                 albedo, metallic, roughness);

    float3 lighting = ambient + direct + emissive;

    for (int pi = 0; pi < uNumPointLights; pi++)
        lighting += CalcPBRPointLight(pointLights[pi], N, V, fragPos,
                                      albedo, metallic, roughness);

    for (int si = 0; si < uNumSpotLights; si++)
        lighting += CalcPBRSpotLight(spotLights[si], N, V, fragPos,
                                     albedo, metallic, roughness);

    return float4(lighting, 1.0);
}
