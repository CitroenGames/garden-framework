// FXAA post-processing shader

cbuffer FXAACB : register(b0)
{
    float2 uInverseScreenSize;
    float2 padding;
};

Texture2D screenTexture : register(t0);
SamplerState linearSampler : register(s0);

struct VSInput
{
    float2 position : POSITION;
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
    output.position = float4(input.position, 0.0, 1.0);
    output.texcoord = float2(input.texcoord.x, 1.0 - input.texcoord.y);
    return output;
}

#define FXAA_SPAN_MAX 8.0
#define FXAA_REDUCE_MUL (1.0 / 8.0)
#define FXAA_REDUCE_MIN (1.0 / 128.0)

float4 PSMain(PSInput input) : SV_TARGET
{
    float2 texCoord = input.texcoord;

    // Sample neighboring pixels
    float3 rgbNW = screenTexture.Sample(linearSampler, texCoord + float2(-1.0, -1.0) * uInverseScreenSize).rgb;
    float3 rgbNE = screenTexture.Sample(linearSampler, texCoord + float2(1.0, -1.0) * uInverseScreenSize).rgb;
    float3 rgbSW = screenTexture.Sample(linearSampler, texCoord + float2(-1.0, 1.0) * uInverseScreenSize).rgb;
    float3 rgbSE = screenTexture.Sample(linearSampler, texCoord + float2(1.0, 1.0) * uInverseScreenSize).rgb;
    float3 rgbM = screenTexture.Sample(linearSampler, texCoord).rgb;

    // Luma conversion
    float3 luma = float3(0.299, 0.587, 0.114);
    float lumaNW = dot(rgbNW, luma);
    float lumaNE = dot(rgbNE, luma);
    float lumaSW = dot(rgbSW, luma);
    float lumaSE = dot(rgbSE, luma);
    float lumaM = dot(rgbM, luma);

    // Calculate min/max luma
    float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));

    // Edge direction
    float2 dir;
    dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    dir.y = ((lumaNW + lumaSW) - (lumaNE + lumaSE));

    // Direction reduction
    float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * (0.25 * FXAA_REDUCE_MUL), FXAA_REDUCE_MIN);
    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);

    // Clamp direction
    dir = min(float2(FXAA_SPAN_MAX, FXAA_SPAN_MAX),
          max(float2(-FXAA_SPAN_MAX, -FXAA_SPAN_MAX),
          dir * rcpDirMin)) * uInverseScreenSize;

    // Sample along edge
    float3 rgbA = 0.5 * (
        screenTexture.Sample(linearSampler, texCoord + dir * (1.0 / 3.0 - 0.5)).rgb +
        screenTexture.Sample(linearSampler, texCoord + dir * (2.0 / 3.0 - 0.5)).rgb);

    float3 rgbB = rgbA * 0.5 + 0.25 * (
        screenTexture.Sample(linearSampler, texCoord + dir * -0.5).rgb +
        screenTexture.Sample(linearSampler, texCoord + dir * 0.5).rgb);

    float lumaB = dot(rgbB, luma);

    // Choose based on luma comparison
    if (lumaB < lumaMin || lumaB > lumaMax)
    {
        return float4(rgbA, 1.0);
    }
    else
    {
        return float4(rgbB, 1.0);
    }
}
