// Skinned lit shader with Cascaded Shadow Maps

cbuffer GlobalCB : register(b0)
{
    matrix uView;
    matrix uProjection;
    matrix uLightSpaceMatrices[4];
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

cbuffer PerObjectCB : register(b1)
{
    matrix uModel;
    matrix uNormalMatrix;
    float3 uColor;
    int uUseTexture;
};

#define MAX_LIGHTS 16

struct PointLightData
{
    float3 position; float range;
    float3 color; float intensity;
    float3 attenuation; float _pad;
};

struct SpotLightData
{
    float3 position; float range;
    float3 direction; float intensity;
    float3 color; float innerCutoff;
    float3 attenuation; float outerCutoff;
};

cbuffer LightCB : register(b3)
{
    PointLightData pointLights[MAX_LIGHTS];
    SpotLightData  spotLights[MAX_LIGHTS];
    int numPointLights;
    int numSpotLights;
    float2 _lightPad;
    float3 uCameraPos;
    float _lightPad2;
};

static const int MAX_BONES = 128;

cbuffer BoneCB : register(b2)
{
    matrix uBones[MAX_BONES];
    int uHasBones;
    float3 _bonePad;
};

Texture2D diffuseTexture : register(t0);
Texture2DArray shadowMapArray : register(t1);
SamplerState linearSampler : register(s0);
SamplerComparisonState shadowSampler : register(s1);

struct VSInput
{
    float3 position    : POSITION;
    float3 normal      : NORMAL;
    float2 texcoord    : TEXCOORD0;
    int4   boneIds     : BLENDINDICES;
    float4 boneWeights : BLENDWEIGHT;
};

struct PSInput
{
    float4 position  : SV_POSITION;
    float3 fragPos   : TEXCOORD0;
    float3 normal    : TEXCOORD1;
    float2 texcoord  : TEXCOORD2;
    float viewDepth  : TEXCOORD3;
};

PSInput VSMain(VSInput input)
{
    PSInput output;

    float4 skinnedPos;
    float3 skinnedNormal;

    if (uHasBones)
    {
        matrix boneTransform = uBones[input.boneIds[0]] * input.boneWeights[0]
                             + uBones[input.boneIds[1]] * input.boneWeights[1]
                             + uBones[input.boneIds[2]] * input.boneWeights[2]
                             + uBones[input.boneIds[3]] * input.boneWeights[3];

        skinnedPos = mul(boneTransform, float4(input.position, 1.0));
        skinnedNormal = mul((float3x3)boneTransform, input.normal);
    }
    else
    {
        skinnedPos = float4(input.position, 1.0);
        skinnedNormal = input.normal;
    }

    float4 worldPos = mul(uModel, skinnedPos);
    output.fragPos = worldPos.xyz;

    // Transform normal using precomputed normal matrix
    output.normal = mul((float3x3)uNormalMatrix, skinnedNormal);

    output.texcoord = input.texcoord;

    float4 viewPos = mul(uView, worldPos);
    output.viewDepth = -viewPos.z;

    output.position = mul(uProjection, viewPos);
    return output;
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

float ShadowCalculation(int cascadeIndex, float3 fragPos, float3 normal)
{
    float4 fragPosLightSpace = mul(uLightSpaceMatrices[cascadeIndex], float4(fragPos, 1.0));
    float3 projCoords = fragPosLightSpace.xyz / fragPosLightSpace.w;

    // D3D11 NDC: X and Y are [-1,1], Z is [0,1]
    projCoords.xy = projCoords.xy * 0.5 + 0.5;
    projCoords.y = 1.0 - projCoords.y;  // Flip Y for texture sampling

    float currentDepth = projCoords.z;
    if (currentDepth > 1.0 || currentDepth < 0.0)
        return 0.0;

    // Bias calculation
    float3 lightDir = normalize(-uLightDir);
    float baseBias = max(0.0005 * (1.0 - dot(normalize(normal), lightDir)), 0.00005);
    float bias = baseBias * (1.0 + float(cascadeIndex) * 0.5);

    // PCF 3x3 using hardware comparison sampler
    float shadow = 0.0;
    float2 texelSize = uShadowMapTexelSize;

    [unroll]
    for (int x = -1; x <= 1; x++)
    {
        [unroll]
        for (int y = -1; y <= 1; y++)
        {
            float2 sampleUV = projCoords.xy + float2(x, y) * texelSize;
            shadow += shadowMapArray.SampleCmpLevelZero(shadowSampler, float3(sampleUV, cascadeIndex), currentDepth - bias);
        }
    }
    return shadow / 9.0;
}

float3 CalcPointLight(PointLightData light, float3 normal, float3 fragPos)
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

float3 CalcSpotLight(SpotLightData light, float3 normal, float3 fragPos)
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

float4 PSMain(PSInput input) : SV_TARGET
{
    float3 norm = normalize(input.normal);
    float3 lightDir = normalize(-uLightDir);
    float diff = max(dot(norm, lightDir), 0.0);

    float3 ambient = uLightAmbient;
    float3 diffuse = uLightDiffuse * diff;

    // CSM shadow
    int cascadeIndex = getCascadeIndex(input.viewDepth);
    float shadow = ShadowCalculation(cascadeIndex, input.fragPos, input.normal);
    float3 lighting = ambient + (1.0 - shadow) * diffuse;

    // Point lights
    for (int i = 0; i < numPointLights; i++)
        lighting += CalcPointLight(pointLights[i], norm, input.fragPos);

    // Spot lights
    for (int j = 0; j < numSpotLights; j++)
        lighting += CalcSpotLight(spotLights[j], norm, input.fragPos);

    float4 texColor;
    if (uUseTexture)
    {
        texColor = diffuseTexture.Sample(linearSampler, input.texcoord);
    }
    else
    {
        texColor = float4(uColor, 1.0);
    }

    // Debug cascade visualization
    if (uDebugCascades)
    {
        float3 cascadeColors[4] = {
            float3(1.0, 0.0, 0.0),
            float3(0.0, 1.0, 0.0),
            float3(0.0, 0.0, 1.0),
            float3(1.0, 1.0, 0.0)
        };
        texColor.rgb = lerp(texColor.rgb, cascadeColors[cascadeIndex], 0.3);
    }

    return float4(lighting * texColor.rgb, texColor.a);
}
