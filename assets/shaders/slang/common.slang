// Common types and utility functions shared across all shaders
module common;

public static const int MAX_LIGHTS = 16;
public static const int MAX_BONES = 128;

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
