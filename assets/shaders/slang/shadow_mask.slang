// Screen-space shadow mask post-process pass
// Reconstructs world position from depth buffer, samples CSM shadow maps,
// and outputs a shadow factor texture (1.0 = lit, 0.0 = shadowed)
import common;

[[vk::binding(2, 0)]]
cbuffer ShadowMaskCB : register(b0)
{
    float4x4 uInvViewProj;
    float4x4 uView;
    float4x4 uLightSpaceMatrices[4];
    float4 uCascadeSplits;
    float uCascadeSplit4;
    int uCascadeCount;
    float2 uShadowMapTexelSize;
    float2 uScreenSize;
    float3 uLightDir;
    float _pad;
};

[[vk::binding(0, 0)]]Texture2D depthTexture : register(t0);
[[vk::binding(0, 0)]]SamplerState depthSampler : register(s0);

[[vk::binding(1, 0)]]Texture2DArray shadowMapArray : register(t1);
[[vk::binding(1, 0)]]SamplerComparisonState shadowSampler : register(s1);

struct VSInput
{
    float2 position : POSITION;
    float2 texcoord : TEXCOORD0;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float2 texcoord : TEXCOORD0;
};

[shader("vertex")]
PSInput vertexMain(VSInput input)
{
    PSInput output;
    output.position = float4(input.position, 0.0, 1.0);
#ifdef TARGET_HLSL
    output.texcoord = float2(input.texcoord.x, 1.0 - input.texcoord.y);
#else
    output.texcoord = input.texcoord;
#endif
    return output;
}

// Reconstruct world-space position from depth buffer
float3 ReconstructWorldPos(float2 uv, float depth)
{
    float2 ndc = uv * 2.0 - 1.0;
#ifdef TARGET_HLSL
    float4 clipPos = float4(ndc.x, -ndc.y, depth, 1.0);
#else
    float4 clipPos = float4(ndc.x, -ndc.y, depth, 1.0);
#endif
    float4 worldPos = mul(uInvViewProj, clipPos);
    return worldPos.xyz / worldPos.w;
}

// Reconstruct world-space normal from depth buffer using finite differences
float3 ReconstructNormal(float2 uv, float2 texelSize, float3 worldPos)
{
    float depthRight = depthTexture.Sample(depthSampler, uv + float2(texelSize.x, 0)).r;
    float depthLeft  = depthTexture.Sample(depthSampler, uv - float2(texelSize.x, 0)).r;
    float depthUp    = depthTexture.Sample(depthSampler, uv + float2(0, texelSize.y)).r;
    float depthDown  = depthTexture.Sample(depthSampler, uv - float2(0, texelSize.y)).r;

    float3 posRight = ReconstructWorldPos(uv + float2(texelSize.x, 0), depthRight);
    float3 posLeft  = ReconstructWorldPos(uv - float2(texelSize.x, 0), depthLeft);
    float3 posUp    = ReconstructWorldPos(uv + float2(0, texelSize.y), depthUp);
    float3 posDown  = ReconstructWorldPos(uv - float2(0, texelSize.y), depthDown);

    // Choose the pair with the smaller depth difference (handles edges better)
    float3 dx = (abs(posRight.z - worldPos.z) < abs(worldPos.z - posLeft.z))
                ? (posRight - worldPos) : (worldPos - posLeft);
    float3 dy = (abs(posUp.z - worldPos.z) < abs(worldPos.z - posDown.z))
                ? (posUp - worldPos) : (worldPos - posDown);

#ifdef TARGET_HLSL
    return normalize(cross(dy, dx));
#else
    return normalize(cross(dx, dy));
#endif
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

float ShadowCalculationSingle(int cascadeIndex, float3 fragPos, float3 normal, float2 screenPos)
{
    float4 fragPosLightSpace = mul(uLightSpaceMatrices[cascadeIndex], float4(fragPos, 1.0));
    float3 projCoords = fragPosLightSpace.xyz / fragPosLightSpace.w;

    projCoords.xy = projCoords.xy * 0.5 + 0.5;
#ifdef TARGET_HLSL
    projCoords.y = 1.0 - projCoords.y;
#endif

    float currentDepth = projCoords.z;
    if (currentDepth > 1.0 || currentDepth < 0.0)
        return 1.0;

    float3 lightDir = normalize(-uLightDir);
    float baseBias = max(0.0005 * (1.0 - dot(normalize(normal), lightDir)), 0.00005);
    float bias = baseBias * (1.0 + float(cascadeIndex) * 0.5);

    // Per-pixel rotation angle for Poisson disc (reduces banding)
    float angle = frac(sin(dot(screenPos, float2(12.9898, 78.233))) * 43758.5453) * 6.28318530718;
    float cosA = cos(angle);
    float sinA = sin(angle);

    // 16-sample Poisson disc PCF with per-pixel rotation
    float shadow = 0.0;
    float2 texelSize = uShadowMapTexelSize;
    float spread = 1.5;

    [unroll]
    for (int i = 0; i < POISSON_SAMPLE_COUNT; i++)
    {
        float2 offset = poissonDisk[i];
        float2 rotated = float2(
            offset.x * cosA - offset.y * sinA,
            offset.x * sinA + offset.y * cosA
        );
        float2 sampleUV = projCoords.xy + rotated * texelSize * spread;
        shadow += shadowMapArray.SampleCmpLevelZero(shadowSampler, float3(sampleUV, cascadeIndex), currentDepth - bias);
    }
    return shadow / float(POISSON_SAMPLE_COUNT);
}

float ShadowCalculation(int cascadeIndex, float3 fragPos, float3 normal, float viewDepth, float2 screenPos)
{
    float shadow = ShadowCalculationSingle(cascadeIndex, fragPos, normal, screenPos);

    // Cascade blending: smooth transition between adjacent cascades
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
            float nextShadow = ShadowCalculationSingle(cascadeIndex + 1, fragPos, normal, screenPos);
            shadow = lerp(shadow, nextShadow, blendFactor);
        }
    }

    return shadow;
}

[shader("fragment")]
float4 fragmentMain(PSInput input) : SV_TARGET
{
    float2 uv = input.texcoord;

    float depth = depthTexture.Sample(depthSampler, uv).r;

    // Skip sky pixels
    if (depth >= 0.9999)
        return float4(1.0, 1.0, 1.0, 1.0);

    float2 depthTexelSize = float2(1.0, 1.0) / uScreenSize;

    // Reconstruct world-space position and normal from depth
    float3 worldPos = ReconstructWorldPos(uv, depth);
    float3 normal = ReconstructNormal(uv, depthTexelSize, worldPos);

    // Compute view depth for cascade selection
    float4 viewPos = mul(uView, float4(worldPos, 1.0));
    float viewDepth = -viewPos.z;

    // Screen position for per-pixel Poisson rotation
    float2 screenPos = uv * uScreenSize;

    int cascadeIndex = getCascadeIndex(viewDepth);
    float shadow = ShadowCalculation(cascadeIndex, worldPos, normal, viewDepth, screenPos);

    return float4(shadow, shadow, shadow, 1.0);
}
