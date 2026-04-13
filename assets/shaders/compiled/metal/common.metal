// Shared types and utility functions for Metal shaders

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

// Point/spot light structures matching CPU-side LightCBuffer layout
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
    PointLightData pointLights[16];
    SpotLightData  spotLights[16];
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
