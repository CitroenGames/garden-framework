#include <metal_stdlib>
using namespace metal;

struct RmlUniforms {
    float4x4 transform;
    float2 translation;
};

struct RmlVertexIn {
    float2 position [[attribute(0)]];
    float4 color    [[attribute(1)]];
    float2 texcoord [[attribute(2)]];
};

struct RmlVertexOut {
    float4 position [[position]];
    float4 color;
    float2 texcoord;
};

vertex RmlVertexOut rmlui_vertex(RmlVertexIn in [[stage_in]],
                                 constant RmlUniforms& uniforms [[buffer(1)]]) {
    RmlVertexOut out;
    float2 translated = in.position + uniforms.translation;
    out.position = uniforms.transform * float4(translated, 0.0, 1.0);
    out.color = in.color;
    out.texcoord = in.texcoord;
    return out;
}

fragment float4 rmlui_fragment_textured(RmlVertexOut in [[stage_in]],
                                         texture2d<float> tex [[texture(0)]],
                                         sampler samp [[sampler(0)]]) {
    return in.color * tex.sample(samp, in.texcoord);
}

fragment float4 rmlui_fragment_color(RmlVertexOut in [[stage_in]]) {
    return in.color;
}
