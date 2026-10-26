// Common types and utility functions shared across all shaders
module common;

public static const int MAX_LIGHTS = 16;
public static const int MAX_BONES = 128;

// 16-sample Poisson disc for soft shadow PCF sampling
public static const int POISSON_SAMPLE_COUNT = 16;
public static const float2 poissonDisk[16] = {
    float2(-0.94201624, -0.39906216),
    float2( 0.94558609, -0.76890725),
    float2(-0.09418410, -0.92938870),
    float2( 0.34495938,  0.29387760),
    float2(-0.91588581,  0.45771432),
    float2(-0.81544232, -0.87912464),
    float2(-0.38277543,  0.27676845),
    float2( 0.97484398,  0.75648379),
    float2( 0.44323325, -0.97511554),
    float2( 0.53742981, -0.47373420),
    float2(-0.26496911, -0.41893023),
    float2( 0.79197514,  0.19090188),
    float2(-0.24188840,  0.99706507),
    float2(-0.81409955,  0.91437590),
    float2( 0.19984126,  0.78641367),
    float2( 0.14383161, -0.14100790)
};

public struct PointLightData
{
    public float3 position; public float range;
    public float3 color;    public float intensity;
    public float3 attenuation; public float _pad;
};

public struct SpotLightData
{
    public float3 position;  public float range;
    public float3 direction; public float intensity;
    public float3 color;     public float innerCutoff;
    public float3 attenuation; public float outerCutoff;
};

public float3 CalcPointLight(PointLightData light, float3 normal, float3 fragPos)
{
    float3 toLight = light.position - fragPos;
    float distance = length(toLight);
    if (distance > light.range) return float3(0, 0, 0);
    toLight = normalize(toLight);

    float diff = max(dot(normal, toLight), 0.0);
    float atten = 1.0 / (light.attenuation.x + light.attenuation.y * distance
                         + light.attenuation.z * distance * distance);

    return light.color * light.intensity * diff * atten;
}

public float3 CalcSpotLight(SpotLightData light, float3 normal, float3 fragPos)
{
    float3 toLight = light.position - fragPos;
    float distance = length(toLight);
    if (distance > light.range) return float3(0, 0, 0);
    toLight = normalize(toLight);

    float theta = dot(toLight, normalize(-light.direction));
    float epsilon = light.innerCutoff - light.outerCutoff;
    float spotIntensity = saturate((theta - light.outerCutoff) / epsilon);

    float diff = max(dot(normal, toLight), 0.0);
    float atten = 1.0 / (light.attenuation.x + light.attenuation.y * distance
                         + light.attenuation.z * distance * distance);

    return light.color * light.intensity * diff * atten * spotIntensity;
}

// One cell of the Forward+ light grid (16 bytes, mirrors ForwardPlusTile in
// Engine/src/Graphics/ForwardPlusLightBinning.hpp). Element 0 of the tile
// buffer is a grid header with the same layout: lightOffset = sliceCount,
// lightCount = bits of zScale, padding0 = bits of zOffset,
// padding1 = bits of zDistribution. Cells follow it, slice-major:
// cell = (slice * tilesY + tileY) * tilesX + tileX, at element cell + 1.
public struct ForwardPlusTile
{
    public uint lightOffset;
    public uint lightCount;
    public uint padding0;
    public uint padding1;
};

// Slice of a view depth (positive in front, -viewZ) for the grid in `header`,
// mirroring forwardPlusGridSlice() in ForwardPlusLightBinning.hpp.
public uint forwardPlusGridSlice(ForwardPlusTile header, float viewDepth)
{
    uint sliceCount = max(header.lightOffset, 1u);
    if (sliceCount <= 1u) return 0u;
    float term = viewDepth * asfloat(header.lightCount) + asfloat(header.padding0);
    float s = log2(max(term, 1.0f)) * asfloat(header.padding1);
    return min(uint(max(s, 0.0f)), sliceCount - 1u);
}
