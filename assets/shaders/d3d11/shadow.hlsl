// Shadow pass shader - depth only

cbuffer ShadowCB : register(b0)
{
    matrix uLightSpaceMatrix;
    matrix uModel;
};

struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 texcoord : TEXCOORD0;
};

struct PSInput
{
    float4 position : SV_POSITION;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    float4 worldPos = mul(uModel, float4(input.position, 1.0));
    output.position = mul(uLightSpaceMatrix, worldPos);
    return output;
}

// Empty pixel shader - only depth is written
void PSMain(PSInput input)
{
    // Depth-only pass - no color output needed
}
