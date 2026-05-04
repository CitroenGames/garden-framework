// Shared types and utility functions for Metal shaders

kernel void metal_structured_lights_v1() {}

struct GlobalUBO {
    float4x4 view;
    float4x4 projection;
    float4x4 lightSpaceMatrices[4];
    float4 cascadeSplits;
    packed_float3 lightDir; float cascadeSplit4;
    packed_float3 lightAmbient; int cascadeCount;
    packed_float3 lightDiffuse; int debugCascades;
    packed_float3 color; int useTexture;
    float alphaCutoff;
    float _globalPad1;
    float2 shadowMapTexelSize;
    float metallic;
    float roughness;
    float2 _pbrPad;
    packed_float3 emissive;
    float _pbrPad2;
};

struct ShadowUBO {
    float4x4 lightSpaceMatrix;
};

struct BoneUBO {
    float4x4 bones[128];
    int hasBones;
};

struct ModelData {
    float4x4 model;
    float4x4 normalMatrix;
};

// Point/spot light structures matching CPU-side GPUPointLight/GPUSpotLight layout
struct PointLightData {
    packed_float3 position; float range;
    packed_float3 color;    float intensity;
    packed_float3 attenuation; float _pad;
};

struct SpotLightData {
    packed_float3 position;  float range;
    packed_float3 direction; float intensity;
    packed_float3 color;     float innerCutoff;
    packed_float3 attenuation; float outerCutoff;
};

struct LightCBuffer {
    int numPointLights;
    int numSpotLights;
    float _pad[2];
    packed_float3 cameraPos;
    float _pad2;
};

float3 CalcPointLight(PointLightData light, float3 normal, float3 fragPos)
{
    float3 toLight = float3(light.position) - fragPos;
    float distance = length(toLight);
    if (distance > light.range) return float3(0, 0, 0);
    toLight = normalize(toLight);

    float diff = max(dot(normal, toLight), 0.0);
    float3 atten_vec = float3(light.attenuation);
    float atten = 1.0 / (atten_vec.x + atten_vec.y * distance
                         + atten_vec.z * distance * distance);

    return float3(light.color) * light.intensity * diff * atten;
}

float3 CalcSpotLight(SpotLightData light, float3 normal, float3 fragPos)
{
    float3 toLight = float3(light.position) - fragPos;
    float distance = length(toLight);
    if (distance > light.range) return float3(0, 0, 0);
    toLight = normalize(toLight);

    float theta = dot(toLight, normalize(-float3(light.direction)));
    float epsilon = light.innerCutoff - light.outerCutoff;
    float spotIntensity = saturate((theta - light.outerCutoff) / epsilon);

    float diff = max(dot(normal, toLight), 0.0);
    float3 atten_vec = float3(light.attenuation);
    float atten = 1.0 / (atten_vec.x + atten_vec.y * distance
                         + atten_vec.z * distance * distance);

    return float3(light.color) * light.intensity * diff * atten * spotIntensity;
}

constant float PBR_PI = 3.14159265359;

float DistributionGGX(float3 N, float3 H, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    float denom = NdotH2 * (a2 - 1.0) + 1.0;
    denom = PBR_PI * denom * denom;
    return a2 / max(denom, 0.0000001);
}

float GeometrySchlickGGX(float NdotV, float roughness)
{
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float GeometrySmith(float3 N, float3 V, float3 L, float roughness)
{
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    return GeometrySchlickGGX(NdotV, roughness) * GeometrySchlickGGX(NdotL, roughness);
}

float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (float3(1.0) - F0) * pow(saturate(1.0 - cosTheta), 5.0);
}

float PBRAttenuation(float distance, float range)
{
    float safeRange = max(range, 0.0001);
    float d2 = distance * distance;
    float ratio = distance / safeRange;
    float ratio2 = ratio * ratio;
    float falloff = saturate(1.0 - ratio2 * ratio2);
    falloff = falloff * falloff;
    return falloff / (d2 + 1.0);
}

float3 CookTorranceBRDF(float3 N, float3 V, float3 L, float3 lightColor,
                        float3 albedo, float metallic, float roughness)
{
    float3 H = normalize(V + L);
    float NdotL = max(dot(N, L), 0.0);
    if (NdotL <= 0.0)
        return float3(0.0);

    float3 F0 = mix(float3(0.04, 0.04, 0.04), albedo, metallic);
    float D = DistributionGGX(N, H, roughness);
    float G = GeometrySmith(N, V, L, roughness);
    float3 F = FresnelSchlick(max(dot(H, V), 0.0), F0);
    float NdotV = max(dot(N, V), 0.0);
    float3 specular = (D * G * F) / (4.0 * NdotV * NdotL + 0.0001);
    float3 kD = (float3(1.0) - F) * (1.0 - metallic);
    float3 diffuse = kD * albedo / PBR_PI;
    return (diffuse + specular) * lightColor * NdotL;
}

float3 CalcPBRDirectional(float3 N, float3 V, float3 L, float3 lightColor,
                          float3 albedo, float metallic, float roughness)
{
    return CookTorranceBRDF(N, V, L, lightColor, albedo, metallic, roughness);
}

float3 CalcPBRPointLight(PointLightData light, float3 N, float3 V, float3 fragPos,
                         float3 albedo, float metallic, float roughness)
{
    float3 toLight = float3(light.position) - fragPos;
    float distance = length(toLight);
    if (distance > light.range) return float3(0.0);

    float3 L = toLight / max(distance, 0.0001);
    float atten = PBRAttenuation(distance, light.range);
    return CookTorranceBRDF(N, V, L, float3(light.color) * light.intensity * atten,
                            albedo, metallic, roughness);
}

float3 CalcPBRSpotLight(SpotLightData light, float3 N, float3 V, float3 fragPos,
                        float3 albedo, float metallic, float roughness)
{
    float3 toLight = float3(light.position) - fragPos;
    float distance = length(toLight);
    if (distance > light.range) return float3(0.0);

    float3 L = toLight / max(distance, 0.0001);
    float theta = dot(L, normalize(-float3(light.direction)));
    float epsilon = max(light.innerCutoff - light.outerCutoff, 0.0001);
    float spotIntensity = saturate((theta - light.outerCutoff) / epsilon);
    float atten = PBRAttenuation(distance, light.range);
    return CookTorranceBRDF(N, V, L, float3(light.color) * light.intensity * atten * spotIntensity,
                            albedo, metallic, roughness);
}

// Get cascade split distance by index
float getCascadeSplit(constant GlobalUBO& ubo, int index)
{
    if (index == 0) return ubo.cascadeSplits.x;
    if (index == 1) return ubo.cascadeSplits.y;
    if (index == 2) return ubo.cascadeSplits.z;
    if (index == 3) return ubo.cascadeSplits.w;
    return ubo.cascadeSplit4;
}

int getCascadeIndex(constant GlobalUBO& ubo, float viewDepth)
{
    if (ubo.cascadeCount <= 0) return 0;

    for (int i = 0; i < ubo.cascadeCount; i++) {
        if (viewDepth < getCascadeSplit(ubo, i + 1)) {
            return i;
        }
    }
    return max(ubo.cascadeCount - 1, 0);
}

float ShadowCalculation(constant GlobalUBO& ubo, float3 fragPos, float3 normal,
                          int cascadeIndex, depth2d_array<float> shadowMap, sampler shadowSampler)
{
    float4 fragPosLightSpace = ubo.lightSpaceMatrices[cascadeIndex] * float4(fragPos, 1.0);
    float3 projCoords = fragPosLightSpace.xyz / fragPosLightSpace.w;
    projCoords.xy = projCoords.xy * 0.5 + 0.5;
    projCoords.y = 1.0 - projCoords.y; // Metal Y flip

    float currentDepth = projCoords.z;

    if (projCoords.z > 1.0) return 0.0;

    float3 lightDir = normalize(-ubo.lightDir);
    float baseBias = max(0.0005 * (1.0 - dot(normalize(normal), lightDir)), 0.00005);
    float bias = baseBias * (1.0 + float(cascadeIndex) * 0.5);
    float biasedDepth = currentDepth - bias;

    // PCF 3x3 using hardware sample_compare (faster than manual comparison)
    float shadow = 0.0;
    float2 texelSize = ubo.shadowMapTexelSize;
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            shadow += shadowMap.sample_compare(shadowSampler, projCoords.xy + float2(x, y) * texelSize, cascadeIndex, biasedDepth);
        }
    }
    shadow /= 9.0;

    return shadow;
}

float ShadowCalculationWithBlend(constant GlobalUBO& ubo, float3 fragPos, float3 normal, float viewDepth,
                                   depth2d_array<float> shadowMap, sampler shadowSampler)
{
    int cascadeIndex = getCascadeIndex(ubo, viewDepth);
    float shadow = ShadowCalculation(ubo, fragPos, normal, cascadeIndex, shadowMap, shadowSampler);

    float blendRange = 0.1;
    float cascadeEnd = getCascadeSplit(ubo, cascadeIndex + 1);
    float cascadeStart = getCascadeSplit(ubo, cascadeIndex);
    float cascadeRange = cascadeEnd - cascadeStart;
    float distToEnd = cascadeEnd - viewDepth;

    if (cascadeIndex < ubo.cascadeCount - 1 && distToEnd < cascadeRange * blendRange) {
        float blendFactor = distToEnd / (cascadeRange * blendRange);
        float nextShadow = ShadowCalculation(ubo, fragPos, normal, cascadeIndex + 1, shadowMap, shadowSampler);
        shadow = mix(nextShadow, shadow, blendFactor);
    }

    return shadow;
}
