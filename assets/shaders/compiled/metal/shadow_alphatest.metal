#include <metal_stdlib>
using namespace metal;

// Shadow pass shader with alpha test - samples texture and discards transparent fragments

struct ShadowAlphaVertexIn {
    float3 position [[attribute(0)]];
    float3 normal   [[attribute(1)]];
    float2 texCoord [[attribute(2)]];
};

struct ShadowAlphaVertexOut {
    float4 position [[position]];
    float2 texCoord;
};

struct ShadowUBO {
    float4x4 lightSpaceMatrix;
};

vertex ShadowAlphaVertexOut shadow_alphatest_vertex(
    ShadowAlphaVertexIn in [[stage_in]],
    constant ShadowUBO& ubo [[buffer(1)]],
    constant float4x4& model [[buffer(2)]])
{
    ShadowAlphaVertexOut out;
    out.position = ubo.lightSpaceMatrix * model * float4(in.position, 1.0);
    out.texCoord = in.texCoord;
    return out;
}

fragment void shadow_alphatest_fragment(
    ShadowAlphaVertexOut in [[stage_in]],
    texture2d<float> tex [[texture(0)]],
    sampler texSampler [[sampler(0)]])
{
    float4 texColor = tex.sample(texSampler, in.texCoord);
    if (texColor.a < 0.5)
        discard_fragment();
}
