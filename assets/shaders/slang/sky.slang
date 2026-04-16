// Procedural sky shader (fullscreen post-process quad)
// Reconstructs view direction from depth buffer + inverse view-projection matrix

[[vk::binding(1, 0)]]
cbuffer SkyboxCB : register(b0)
{
    float4x4 uInvViewProj;
    float3 uSunDirection;
    float _pad;
};

[[vk::binding(0, 0)]]Texture2D depthTexture : register(t0);
[[vk::binding(0, 0)]]SamplerState depthSampler : register(s0);

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

// Sky color constants
static const float3 SKY_TOP = float3(0.15, 0.35, 0.65);
static const float3 SKY_HORIZON = float3(0.55, 0.70, 0.90);
static const float3 SKY_BOTTOM = float3(0.75, 0.85, 0.95);
static const float3 SUN_COLOR = float3(1.0, 0.95, 0.85);
static const float3 SUN_HALO_COLOR = float3(1.0, 0.7, 0.4);
static const float3 SUNSET_COLOR = float3(1.0, 0.5, 0.2);
static const float3 SUNSET_HORIZON = float3(0.9, 0.4, 0.3);

[shader("vertex")]
PSInput vertexMain(VSInput input)
{
    PSInput output;
    output.position = float4(input.position, 1.0, 1.0);
#ifdef TARGET_HLSL
    // D3D: flip Y for texture sampling
    output.texcoord = float2(input.texcoord.x, 1.0 - input.texcoord.y);
#else
    output.texcoord = input.texcoord;
#endif
    return output;
}

[shader("fragment")]
float4 fragmentMain(PSInput input) : SV_TARGET
{
    // Sample depth; discard if geometry is present (depth < 1.0)
    // Vulkan uses hardware depth test (z=1.0 + LESS_OR_EQUAL) instead
#ifdef TARGET_HLSL
    float depth = depthTexture.Sample(depthSampler, input.texcoord).r;
    if (depth < 1.0)
        discard;
#endif

    // Reconstruct view direction from screen UV
    float2 ndc = input.texcoord * 2.0 - 1.0;
#ifdef TARGET_HLSL
    float4 clipPos = float4(ndc.x, -ndc.y, 1.0, 1.0);
#else
    float4 clipPos = float4(ndc.x, -ndc.y, 1.0, 1.0);
#endif
    float4 worldDir = mul(uInvViewProj, clipPos);
    float3 dir = normalize(worldDir.xyz / worldDir.w);

    float3 sunDir = normalize(uSunDirection);
    float sunHeight = sunDir.y;

    // Sky gradient based on view direction
    float3 color;
    if (dir.y > 0.0)
    {
        float t = pow(dir.y, 0.5);
        color = lerp(SKY_HORIZON, SKY_TOP, t);
    }
    else
    {
        float t = pow(-dir.y, 0.8);
        color = lerp(SKY_HORIZON, SKY_BOTTOM * 0.5, t);
    }

    // Sunset coloring when sun is near horizon
    float sunsetFactor = 1.0 - smoothstep(-0.1, 0.3, sunHeight);
    if (sunsetFactor > 0.0)
    {
        float horizonFactor = 1.0 - abs(dir.y);
        horizonFactor = pow(horizonFactor, 2.0);
        color = lerp(color, SUNSET_HORIZON, horizonFactor * sunsetFactor * 0.6);
    }

    // Sun disc
    float sunAngle = dot(dir, sunDir);
    float sunDisc = smoothstep(0.9995, 0.9998, sunAngle);

    // Sun glow/halo
    float innerGlow = pow(max(0.0, sunAngle), 256.0) * 2.0;
    float outerGlow = pow(max(0.0, sunAngle), 8.0) * 0.4;
    float sunsetBoost = 1.0 + (1.0 - smoothstep(-0.1, 0.3, sunHeight)) * 1.5;

    float3 glowColor = lerp(SUN_HALO_COLOR, SUNSET_COLOR, 1.0 - smoothstep(-0.1, 0.2, sunHeight));
    color += glowColor * (innerGlow + outerGlow) * sunsetBoost;

    // Add sun disc
    color = lerp(color, SUN_COLOR * 2.0, sunDisc);

    // Output linear HDR -- FXAA pass handles tone mapping and gamma correction
    return float4(color, 1.0);
}
