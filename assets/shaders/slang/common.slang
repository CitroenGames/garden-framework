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
    float3 position; float range;
    float3 color;    float intensity;
    float3 attenuation; float _pad;
};

public struct SpotLightData
{
    float3 position;  float range;
    float3 direction; float intensity;
    float3 color;     float innerCutoff;
    float3 attenuation; float outerCutoff;
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
