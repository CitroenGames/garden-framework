#include <metal_stdlib>
using namespace metal;

struct SkyboxUBO {
    float4x4 view;
    float4x4 projection;
    packed_float3 sunDirection;
    float time;
};

struct SkyVertexIn {
    float3 position [[attribute(0)]];
};

struct SkyVertexOut {
    float4 position [[position]];
    float3 worldPos;
};

vertex SkyVertexOut sky_vertex(SkyVertexIn in [[stage_in]],
                                constant SkyboxUBO& ubo [[buffer(1)]])
{
    SkyVertexOut out;
    out.worldPos = in.position;

    // Remove translation from view matrix
    float4x4 viewNoTranslation = ubo.view;
    viewNoTranslation[3] = float4(0, 0, 0, 1);

    float4 pos = ubo.projection * viewNoTranslation * float4(in.position, 1.0);
    out.position = float4(pos.xy, pos.w, pos.w); // Force depth to maximum (z = w)

    return out;
}

// Sky colors
constant float3 SKY_TOP = float3(0.15, 0.35, 0.65);
constant float3 SKY_HORIZON = float3(0.55, 0.70, 0.90);
constant float3 SKY_BOTTOM = float3(0.75, 0.85, 0.95);
constant float3 SUN_COLOR = float3(1.0, 0.95, 0.85);
constant float3 SUN_HALO_COLOR = float3(1.0, 0.7, 0.4);
constant float3 SUNSET_COLOR = float3(1.0, 0.5, 0.2);
constant float3 SUNSET_HORIZON = float3(0.9, 0.4, 0.3);

float3 getSkyGradient(float3 dir, float sunHeight)
{
    float y = dir.y;
    float3 skyColor;
    if (y > 0.0) {
        float t = pow(y, 0.5);
        skyColor = mix(SKY_HORIZON, SKY_TOP, t);
    } else {
        float t = pow(-y, 0.8);
        skyColor = mix(SKY_HORIZON, SKY_BOTTOM * 0.5, t);
    }

    float sunsetFactor = 1.0 - smoothstep(-0.1, 0.3, sunHeight);
    if (sunsetFactor > 0.0) {
        float horizonFactor = 1.0 - abs(y);
        horizonFactor = pow(horizonFactor, 2.0);
        skyColor = mix(skyColor, SUNSET_HORIZON, horizonFactor * sunsetFactor * 0.6);
    }

    return skyColor;
}

float getSunDisc(float3 dir, float3 sunDir)
{
    float sunAngle = dot(dir, sunDir);
    return smoothstep(0.9995, 0.9998, sunAngle);
}

float3 getSunGlow(float3 dir, float3 sunDir, float sunHeight)
{
    float sunAngle = dot(dir, sunDir);
    float innerGlow = pow(max(0.0, sunAngle), 256.0) * 2.0;
    float outerGlow = pow(max(0.0, sunAngle), 8.0) * 0.4;
    float sunsetBoost = 1.0 + (1.0 - smoothstep(-0.1, 0.3, sunHeight)) * 1.5;
    float3 glowColor = mix(SUN_HALO_COLOR, SUNSET_COLOR, 1.0 - smoothstep(-0.1, 0.2, sunHeight));
    return glowColor * (innerGlow + outerGlow) * sunsetBoost;
}

fragment float4 sky_fragment(SkyVertexOut in [[stage_in]],
                              constant SkyboxUBO& ubo [[buffer(0)]])
{
    float3 dir = normalize(in.worldPos);
    float3 sunDir = normalize(ubo.sunDirection);
    float sunHeight = sunDir.y;

    float3 color = getSkyGradient(dir, sunHeight);
    color += getSunGlow(dir, sunDir, sunHeight);

    float sunDisc = getSunDisc(dir, sunDir);
    color = mix(color, SUN_COLOR * 2.0, sunDisc);

    // Tone mapping
    color = color / (color + float3(1.0));

    // Gamma correction
    color = pow(color, float3(1.0 / 2.2));

    return float4(color, 1.0);
}
