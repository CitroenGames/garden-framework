// Shadow pass shader with alpha test - samples texture and discards transparent fragments

[[vk::binding(0, 0)]]
cbuffer ShadowCB : register(b0)
{
#ifndef TARGET_SPIRV
    float4x4 uLightSpaceMatrix;  // D3D12: light space in UBO
    float4x4 uModel;             // D3D12: model in UBO
#endif
};

#ifdef TARGET_SPIRV
[[vk::push_constant]]
cbuffer ShadowPush
{
    float4x4 uLightSpaceMatrix;  // Vulkan: light space via push constant (per-cascade)
    float4x4 uModel;             // Vulkan: model via push constant (per-draw)
    float uAlphaCutoff;          // Vulkan: alpha cutoff via push constant (per-draw)
};
#endif

// Per-object CB for alpha cutoff (D3D12 only - Vulkan uses push constants)
#ifndef TARGET_SPIRV
cbuffer PerObjectCB : register(b1)
{
    float4x4 _model_unused;
    float4x4 _normalMatrix_unused;
    float3 _color_unused;
    int _useTexture_unused;
    float uAlphaCutoff;
    float3 _objPad;
};
#endif

Texture2D diffuseTexture : register(t0);
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
    output.position = mul(uLightSpaceMatrix, worldPos);
    output.texcoord = input.texcoord;
    return output;
}

[shader("fragment")]
void fragmentMain(PSInput input)
{
    float4 texColor = diffuseTexture.Sample(linearSampler, input.texcoord);
    if (uAlphaCutoff > 0.0 && texColor.a < uAlphaCutoff)
        discard;
}
