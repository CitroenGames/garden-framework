// Basic lit shader with PBR (Cook-Torrance), Cascaded Shadow Maps, and dynamic lighting
import common;
import pbr;

[[vk::binding(0, 0)]]
cbuffer GlobalCB : register(b0)
{
    float4x4 uView;
    float4x4 uProjection;
    float4x4 uLightSpaceMatrices[4];
    float4 uCascadeSplits;
    float3 uLightDir;
    float uCascadeSplit4;
    float3 uLightAmbient;
    int uCascadeCount;
    float3 uLightDiffuse;
    int uDebugCascades;
    float2 uShadowMapTexelSize;
    float2 _shadowPad;
};

[[vk::binding(4, 0)]]
cbuffer PerObjectCB : register(b1)
{
    float4x4 uModel;
    float4x4 uNormalMatrix;
    float3 uColor;
    int uUseTexture;
    float uAlphaCutoff;
    float uMetallic;
    float uRoughness;
    float _pbrPad1;
    float3 uEmissive;
    float _pbrPad2;
    int uHasMetallicRoughnessMap;
    int uHasNormalMap;
    int uHasOcclusionMap;
    int uHasEmissiveMap;
    int uUseHeightmapDisplacement;
    float uHeightmapScale;
    float uHeightmapOffset;
    float _heightmapPad0;
    float2 uHeightmapTexelSize;
    float2 _heightmapPad1;
    int uMaterialFlags;
    float3 _materialPad0;
#ifdef TARGET_SPIRV
    int uUseInstanceData;
    uint uInstanceBase;
    float2 _instancePad;
#endif
};

static const int MATERIAL_FLAG_WATER = 1;

[[vk::binding(3, 0)]]
cbuffer LightCB : register(b3)
{
    int numPointLights;
    int numSpotLights;
    float2 _lightPad;
    float3 uCameraPos;
    float _lightPad2;
};

[[vk::binding(10, 0)]]StructuredBuffer<PointLightData> pointLights : register(t6);
[[vk::binding(11, 0)]]StructuredBuffer<SpotLightData>  spotLights  : register(t7);

#ifdef TARGET_SPIRV
struct InstanceData
{
    float4x4 model;
    float4x4 normalMatrix;
};
[[vk::binding(12, 0)]]StructuredBuffer<InstanceData> instanceData : register(t9);
#endif

// Base color / diffuse texture
[[vk::binding(1, 0)]]Texture2D diffuseTexture : register(t0);
[[vk::binding(1, 0)]]SamplerState linearSampler : register(s0);

// Shadow map
[[vk::binding(2, 0)]]Texture2DArray shadowMapArray : register(t1);
[[vk::binding(2, 0)]]SamplerComparisonState shadowSampler : register(s1);

// PBR textures
[[vk::binding(6, 0)]]Texture2D metallicRoughnessTexture : register(t2);
[[vk::binding(6, 0)]]SamplerState metallicRoughnessSampler : register(s2);

[[vk::binding(7, 0)]]Texture2D normalTexture : register(t3);
[[vk::binding(7, 0)]]SamplerState normalSampler : register(s3);

[[vk::binding(8, 0)]]Texture2D occlusionTexture : register(t4);
[[vk::binding(8, 0)]]SamplerState occlusionSampler : register(s4);

[[vk::binding(9, 0)]]Texture2D emissiveTexture : register(t5);
[[vk::binding(9, 0)]]SamplerState emissiveSampler : register(s5);

[[vk::binding(13, 0)]]Texture2D heightmapTexture : register(t8);

struct VSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float2 texcoord : TEXCOORD0;
    float4 tangent  : TANGENT;
};

struct PSInput
{
    float4 position  : SV_POSITION;
    float3 fragPos   : TEXCOORD0;
    float3 normal    : TEXCOORD1;
    float2 texcoord  : TEXCOORD2;
    float viewDepth  : TEXCOORD3;
    float3 tangent   : TEXCOORD4;
    float3 bitangent : TEXCOORD5;
};

float sampleHeightmap(float2 uv)
{
    float2 texelSize = max(uHeightmapTexelSize, float2(0.000001, 0.000001));
    float2 dimensions = max(round(1.0 / texelSize), float2(1.0, 1.0));
    int2 coord = int2(round(saturate(uv) * (dimensions - 1.0)));
    return heightmapTexture.Load(int3(coord, 0)).r * uHeightmapScale + uHeightmapOffset;
}

float3 applyHeightmapDisplacement(float3 position, float2 uv)
{
    if (uUseHeightmapDisplacement != 0)
        position.y += sampleHeightmap(uv);
    return position;
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
    float spread = 1.5; // Poisson spread factor

    [unroll]
    for (int i = 0; i < POISSON_SAMPLE_COUNT; i++)
    {
        float2 offset = poissonDisk[i];
        // Rotate the sample
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
        float blendRange = cascadeEnd * 0.1; // Blend over 10% of cascade range
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

[shader("vertex")]
#ifdef TARGET_SPIRV
PSInput vertexMain(VSInput input, uint instanceId : SV_InstanceID)
#else
PSInput vertexMain(VSInput input)
#endif
{
    PSInput output;

#ifdef TARGET_SPIRV
    float4x4 model = uModel;
    float4x4 normalMatrix = uNormalMatrix;
    if (uUseInstanceData != 0)
    {
        InstanceData inst = instanceData[uInstanceBase + instanceId];
        model = inst.model;
        normalMatrix = inst.normalMatrix;
    }
#else
    float4x4 model = uModel;
    float4x4 normalMatrix = uNormalMatrix;
#endif

    float3 localPosition = applyHeightmapDisplacement(input.position, input.texcoord);
    float4 worldPos = mul(model, float4(localPosition, 1.0));
    output.fragPos = worldPos.xyz;

    float3x3 normalMat = (float3x3)normalMatrix;
    output.normal = normalize(mul(normalMat, input.normal));
    output.tangent = normalize(mul(normalMat, input.tangent.xyz));
    output.bitangent = cross(output.normal, output.tangent) * input.tangent.w;

    output.texcoord = input.texcoord;

    float4 viewPos = mul(uView, worldPos);
    output.viewDepth = -viewPos.z;

    output.position = mul(uProjection, viewPos);
    return output;
}

[shader("fragment")]
float4 fragmentMain(PSInput input) : SV_TARGET
{
    // Construct TBN matrix for normal mapping
    float3 T = normalize(input.tangent);
    float3 B = normalize(input.bitangent);
    float3 N = normalize(input.normal);

    // Apply normal map if present
    if (uHasNormalMap != 0)
    {
        float3 normalSample = normalTexture.Sample(normalSampler, input.texcoord).rgb;
        normalSample = normalSample * 2.0 - 1.0; // [0,1] -> [-1,1]
        float3x3 TBN = float3x3(T, B, N);
        N = normalize(mul(normalSample, TBN));
    }

    float3 V = normalize(uCameraPos - input.fragPos);
    float3 lightDir = normalize(-uLightDir);

    // Sample base color or use solid color
    float4 texColor;
    if (uUseTexture != 0)
    {
        texColor = diffuseTexture.Sample(linearSampler, input.texcoord);
        if (uAlphaCutoff > 0.0 && texColor.a < uAlphaCutoff)
            discard;
    }
    else
    {
        texColor = float4(uColor, 1.0);
    }

    float3 albedo = texColor.rgb;
    float metallic = uMetallic;
    float roughness = uRoughness;
    const bool isWater = (uMaterialFlags & MATERIAL_FLAG_WATER) != 0;

    // Sample metallic-roughness map if present (glTF: G=roughness, B=metallic)
    if (uHasMetallicRoughnessMap != 0)
    {
        float4 mrSample = metallicRoughnessTexture.Sample(metallicRoughnessSampler, input.texcoord);
        roughness = mrSample.g * uRoughness;
        metallic = mrSample.b * uMetallic;
    }

    // Clamp roughness to avoid division issues
    roughness = max(roughness, 0.04);

    if (isWater)
    {
        float wave0 = sin(input.fragPos.x * 0.23 + input.fragPos.z * 0.31);
        float wave1 = sin(input.fragPos.x * -0.41 + input.fragPos.z * 0.17);
        N = normalize(N + float3(wave0 * 0.055, 0.0, wave1 * 0.055));

        float fresnel = pow(1.0 - saturate(dot(N, V)), 3.0);
        float depth_tint = saturate(input.viewDepth * 0.018);
        float3 shallow = float3(0.05, 0.32, 0.48);
        float3 deep = float3(0.0, 0.10, 0.22);
        albedo = lerp(shallow, deep, depth_tint);
        albedo = lerp(albedo, float3(0.52, 0.86, 0.95), fresnel * 0.45);
        metallic = 0.0;
        roughness = 0.055;
        texColor.a = max(texColor.a, 0.58 + fresnel * 0.18);
    }

    // Ambient lighting with optional AO
    float ao = 1.0;
    if (uHasOcclusionMap != 0)
    {
        ao = occlusionTexture.Sample(occlusionSampler, input.texcoord).r;
    }
    float3 ambient = uLightAmbient * albedo * ao;

    // PBR directional light
    float3 directLight = CalcPBRDirectional(N, V, lightDir, uLightDiffuse,
                                            albedo, metallic, roughness);

    // Shadow applied as screen-space post-process pass (shadow_mask.slang)
    float shadow = 1.0;
    int cascadeIndex = 0;

    float3 lighting = ambient + shadow * directLight;

    // PBR point lights
    for (int i = 0; i < numPointLights; i++)
        lighting += CalcPBRPointLight(pointLights[i], N, V, input.fragPos,
                                      albedo, metallic, roughness);

    // PBR spot lights
    for (int j = 0; j < numSpotLights; j++)
        lighting += CalcPBRSpotLight(spotLights[j], N, V, input.fragPos,
                                     albedo, metallic, roughness);

    // Emissive
    float3 emissiveColor = uEmissive;
    if (uHasEmissiveMap != 0)
    {
        emissiveColor *= emissiveTexture.Sample(emissiveSampler, input.texcoord).rgb;
    }
    lighting += emissiveColor;

    // Debug cascade visualization
    if (uDebugCascades != 0)
    {
        float3 cascadeColors[4] = {
            float3(1.0, 0.0, 0.0),
            float3(0.0, 1.0, 0.0),
            float3(0.0, 0.0, 1.0),
            float3(1.0, 1.0, 0.0)
        };
        lighting = lerp(lighting, cascadeColors[cascadeIndex], 0.3);
    }

    return float4(lighting, texColor.a);
}
