// RmlUi rendering shader - UI overlay

#ifdef TARGET_SPIRV
[[vk::push_constant]]
cbuffer RmlCB
#else
cbuffer RmlCB : register(b0)
#endif
{
    float4x4 uTransform;
    float2 uTranslation;
    float2 _pad;
};

[[vk::binding(0, 0)]]Texture2D rmlTexture : register(t0);
[[vk::binding(0, 0)]]SamplerState rmlSampler : register(s0);

struct VSInput
{
    float2 position : POSITION;
    float4 color    : COLOR;
    float2 texcoord : TEXCOORD0;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float4 color    : COLOR;
    float2 texcoord : TEXCOORD0;
};

[shader("vertex")]
PSInput vertexMain(VSInput input)
{
    PSInput output;
    float2 p = input.position + uTranslation;
    output.position = mul(uTransform, float4(p, 0.0, 1.0));
    output.color = input.color;
    output.texcoord = input.texcoord;
    return output;
}

[shader("fragment")]
float4 fragmentTextured(PSInput input) : SV_TARGET
{
    return input.color * rmlTexture.Sample(rmlSampler, input.texcoord);
}

[shader("fragment")]
float4 fragmentColor(PSInput input) : SV_TARGET
{
    return input.color;
}
