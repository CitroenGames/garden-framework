// Basic lit shader with Cascaded Shadow Maps and dynamic lighting
import common;

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
};

[[vk::binding(3, 0)]]
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

[[vk::binding(1, 0)]]Texture2D diffuseTexture : register(t0);
[[vk::binding(1, 0)]]SamplerState linearSampler : register(s0);

[[vk::binding(2, 0)]]Texture2DArray shadowMapArray : register(t1);
[[vk::binding(2, 0)]]SamplerComparisonState shadowSampler : register(s1);

struct VSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float2 texcoord : TEXCOORD0;
};

struct PSInput
{
    float4 position  : SV_POSITION;
    float3 fragPos   : TEXCOORD0;
    float3 normal    : TEXCOORD1;
    float2 texcoord  : TEXCOORD2;
    float viewDepth  : TEXCOORD3;
};

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

    // Transform to [0,1] range for texture sampling
    projCoords.xy = projCoords.xy * 0.5 + 0.5;
#ifdef TARGET_HLSL
    projCoords.y = 1.0 - projCoords.y; // Flip Y for D3D11 texture sampling
#endif

    float currentDepth = projCoords.z;
    if (currentDepth > 1.0 || currentDepth < 0.0)
        return 1.0; // Outside cascade frustum — treat as fully lit, not shadowed

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

[shader("vertex")]
PSInput vertexMain(VSInput input)
{
    PSInput output;

    float4 worldPos = mul(uModel, float4(input.position, 1.0));
    output.fragPos = worldPos.xyz;

    // Transform normal using precomputed normal matrix
    output.normal = mul((float3x3)uNormalMatrix, input.normal);

    output.texcoord = input.texcoord;

    float4 viewPos = mul(uView, worldPos);
    output.viewDepth = -viewPos.z; // Negate for right-handed coords

    output.position = mul(uProjection, viewPos);
    return output;
}

[shader("fragment")]
float4 fragmentMain(PSInput input) : SV_TARGET
{
    float3 norm = normalize(input.normal);
    float3 lightDir = normalize(-uLightDir);
    float diff = max(dot(norm, lightDir), 0.0);

    float3 ambient = uLightAmbient;
    float3 diffuse = uLightDiffuse * diff;

    // CSM shadow
    int cascadeIndex = getCascadeIndex(input.viewDepth);
    float shadow = ShadowCalculation(cascadeIndex, input.fragPos, input.normal);
    float3 lighting = ambient + shadow * diffuse;

    // Point lights
    for (int i = 0; i < numPointLights; i++)
        lighting += CalcPointLight(pointLights[i], norm, input.fragPos);

    // Spot lights
    for (int j = 0; j < numSpotLights; j++)
        lighting += CalcSpotLight(spotLights[j], norm, input.fragPos);

    float4 texColor;
    if (uUseTexture != 0)
    {
        texColor = diffuseTexture.Sample(linearSampler, input.texcoord);
    }
    else
    {
        texColor = float4(uColor, 1.0);
    }

    // Debug cascade visualization
    if (uDebugCascades != 0)
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
