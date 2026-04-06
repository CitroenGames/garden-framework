// Shadow pass shader - depth only

[[vk::binding(0, 0)]]
cbuffer ShadowCB : register(b0)
{
    float4x4 uLightSpaceMatrix;
#ifndef TARGET_SPIRV
    float4x4 uModel;       // D3D11/D3D12: model stays in UBO
#endif
};

#ifdef TARGET_SPIRV
[[vk::push_constant]]
cbuffer ShadowPush
{
    float4x4 uModel;       // Vulkan: model via push constant (per-draw)
};
#endif

struct VSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float2 texcoord : TEXCOORD0;
};

struct PSInput
{
    float4 position : SV_POSITION;
};

[shader("vertex")]
PSInput vertexMain(VSInput input)
{
    PSInput output;
    float4 worldPos = mul(uModel, float4(input.position, 1.0));
    output.position = mul(uLightSpaceMatrix, worldPos);
    return output;
}

[shader("fragment")]
void fragmentMain(PSInput input)
{
    // Depth-only pass - no color output needed
}
