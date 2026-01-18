// Unlit shader - no lighting calculations

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
};

cbuffer PerObjectCB : register(b1)
{
    matrix uModel;
    float3 uColor;
    int uUseTexture;
};

Texture2D diffuseTexture : register(t0);
SamplerState linearSampler : register(s0);

struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 texcoord : TEXCOORD0;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float2 texcoord : TEXCOORD0;
};

PSInput VSMain(VSInput input)
{
    PSInput output;

    float4 worldPos = mul(uModel, float4(input.position, 1.0));
    float4 viewPos = mul(uView, worldPos);
    output.position = mul(uProjection, viewPos);
    output.texcoord = input.texcoord;

    return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float4 color;
    if (uUseTexture)
    {
        color = diffuseTexture.Sample(linearSampler, input.texcoord);
    }
    else
    {
        color = float4(uColor, 1.0);
    }
    return color;
}
