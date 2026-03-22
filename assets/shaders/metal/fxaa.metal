#include <metal_stdlib>
using namespace metal;

struct FXAAUniforms {
    float2 inverseScreenSize;
};

struct FXAAVertexIn {
    float2 position [[attribute(0)]];
    float2 texCoord [[attribute(1)]];
};

struct FXAAVertexOut {
    float4 position [[position]];
    float2 texCoord;
};

vertex FXAAVertexOut fxaa_vertex(FXAAVertexIn in [[stage_in]])
{
    FXAAVertexOut out;
    out.texCoord = in.texCoord;
    out.position = float4(in.position, 0.0, 1.0);
    return out;
}

#define FXAA_SPAN_MAX 8.0
#define FXAA_REDUCE_MUL (1.0/8.0)
#define FXAA_REDUCE_MIN (1.0/128.0)

fragment float4 fxaa_fragment(FXAAVertexOut in [[stage_in]],
                               texture2d<float> screenTexture [[texture(0)]],
                               sampler screenSampler [[sampler(0)]],
                               constant FXAAUniforms& uniforms [[buffer(0)]])
{
    float2 texCoords = in.texCoord;
    float2 invScreen = uniforms.inverseScreenSize;

    float3 rgbNW = screenTexture.sample(screenSampler, texCoords + float2(-1.0, -1.0) * invScreen).rgb;
    float3 rgbNE = screenTexture.sample(screenSampler, texCoords + float2( 1.0, -1.0) * invScreen).rgb;
    float3 rgbSW = screenTexture.sample(screenSampler, texCoords + float2(-1.0,  1.0) * invScreen).rgb;
    float3 rgbSE = screenTexture.sample(screenSampler, texCoords + float2( 1.0,  1.0) * invScreen).rgb;
    float3 rgbM  = screenTexture.sample(screenSampler, texCoords).rgb;

    float3 luma = float3(0.299, 0.587, 0.114);
    float lumaNW = dot(rgbNW, luma);
    float lumaNE = dot(rgbNE, luma);
    float lumaSW = dot(rgbSW, luma);
    float lumaSE = dot(rgbSE, luma);
    float lumaM  = dot(rgbM, luma);

    float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));

    float2 dir;
    dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    dir.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));

    float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * (0.25 * FXAA_REDUCE_MUL), FXAA_REDUCE_MIN);
    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);

    dir = min(float2(FXAA_SPAN_MAX), max(float2(-FXAA_SPAN_MAX), dir * rcpDirMin)) * invScreen;

    float3 rgbA = 0.5 * (
        screenTexture.sample(screenSampler, texCoords + dir * (1.0/3.0 - 0.5)).rgb +
        screenTexture.sample(screenSampler, texCoords + dir * (2.0/3.0 - 0.5)).rgb);

    float3 rgbB = rgbA * 0.5 + 0.25 * (
        screenTexture.sample(screenSampler, texCoords + dir * (0.0/3.0 - 0.5)).rgb +
        screenTexture.sample(screenSampler, texCoords + dir * (3.0/3.0 - 0.5)).rgb);

    float lumaB = dot(rgbB, luma);

    if (lumaB < lumaMin || lumaB > lumaMax) {
        return float4(rgbA, 1.0);
    } else {
        return float4(rgbB, 1.0);
    }
}
