// Procedural sky shader

cbuffer SkyboxCB : register(b0)
{
    matrix uProjection;
    matrix uView;
    float3 uSunDirection;
    float uTime;
};

struct VSInput
{
    float3 position : POSITION;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float3 worldPos : TEXCOORD0;
};

// Sky color constants
static const float3 SKY_TOP = float3(0.15, 0.35, 0.65);
static const float3 SKY_HORIZON = float3(0.55, 0.70, 0.90);
static const float3 SKY_BOTTOM = float3(0.75, 0.85, 0.95);
static const float3 SUN_COLOR = float3(1.0, 0.95, 0.85);
static const float3 SUN_HALO_COLOR = float3(1.0, 0.7, 0.4);
static const float3 SUNSET_COLOR = float3(1.0, 0.5, 0.2);
static const float3 SUNSET_HORIZON = float3(0.9, 0.4, 0.3);

PSInput VSMain(VSInput input)
{
    PSInput output;
    output.worldPos = input.position;

    // Remove translation from view matrix
    matrix viewNoTranslation = uView;
    viewNoTranslation[3][0] = 0;
    viewNoTranslation[3][1] = 0;
    viewNoTranslation[3][2] = 0;

    float4 pos = mul(uProjection, mul(viewNoTranslation, float4(input.position, 1.0)));
    output.position = pos.xyww;  // Force depth to maximum (far plane)

    return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float3 dir = normalize(input.worldPos);
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

    // Simple tone mapping
    color = color / (color + float3(1.0, 1.0, 1.0));

    // Gamma correction
    color = pow(color, 1.0 / 2.2);

    return float4(color, 1.0);
}
