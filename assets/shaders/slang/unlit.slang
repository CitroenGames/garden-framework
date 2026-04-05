// Unlit shader - no lighting calculations

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

[[vk::binding(1, 0)]] [[vk::combinedImageSampler]]
Texture2D diffuseTexture : register(t0);
[[vk::binding(1, 0)]] [[vk::combinedImageSampler]]
SamplerState linearSampler : register(s0);

struct VSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float2 texcoord : TEXCOORD0;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float2 texcoord : TEXCOORD0;
};

[shader("vertex")]
PSInput vertexMain(VSInput input)
{
    PSInput output;

    float4 worldPos = mul(uModel, float4(input.position, 1.0));
    float4 viewPos = mul(uView, worldPos);
    output.position = mul(uProjection, viewPos);
    output.texcoord = input.texcoord;

    return output;
}

[shader("fragment")]
float4 fragmentMain(PSInput input) : SV_TARGET
{
    float4 color;
    if (uUseTexture != 0)
    {
        color = diffuseTexture.Sample(linearSampler, input.texcoord);
    }
    else
    {
        color = float4(uColor, 1.0);
    }
    return color;
}
