// FXAA post-processing shader with ACES tone mapping

[[vk::binding(1, 0)]]
cbuffer FXAACB : register(b0)
{
    float2 uInverseScreenSize;
    float uExposure;
    int uSSAOEnabled;
};

[[vk::binding(0, 0)]]Texture2D screenTexture : register(t0);
[[vk::binding(0, 0)]]SamplerState linearSampler : register(s0);

// SSAO texture reuses linearSampler (s0) — s1 is a comparison sampler in the main root signature
[[vk::binding(2, 0)]]Texture2D ssaoTexture : register(t1);

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

[shader("vertex")]
PSInput vertexMain(VSInput input)
{
    PSInput output;
    output.position = float4(input.position, 0.0, 1.0);
#ifdef TARGET_HLSL
    // D3D: flip Y for texture sampling
    output.texcoord = float2(input.texcoord.x, 1.0 - input.texcoord.y);
#else
    output.texcoord = input.texcoord;
#endif
    return output;
}

#define FXAA_SPAN_MAX 8.0
#define FXAA_REDUCE_MUL (1.0 / 8.0)
#define FXAA_REDUCE_MIN (1.0 / 128.0)

// ACES filmic tone mapping (Narkowicz 2015 approximation)
float3 ACESFilm(float3 x)
{
    float a = 2.51;
    float b = 0.03;
    float c = 2.43;
    float d = 0.59;
    float e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

[shader("fragment")]
float4 fragmentMain(PSInput input) : SV_TARGET
{
    float2 texCoord = input.texcoord;

    // Sample neighboring pixels (HDR values)
    float3 rgbNW = screenTexture.Sample(linearSampler, texCoord + float2(-1.0, -1.0) * uInverseScreenSize).rgb;
    float3 rgbNE = screenTexture.Sample(linearSampler, texCoord + float2(1.0, -1.0) * uInverseScreenSize).rgb;
    float3 rgbSW = screenTexture.Sample(linearSampler, texCoord + float2(-1.0, 1.0) * uInverseScreenSize).rgb;
    float3 rgbSE = screenTexture.Sample(linearSampler, texCoord + float2(1.0, 1.0) * uInverseScreenSize).rgb;
    float3 rgbM = screenTexture.Sample(linearSampler, texCoord).rgb;

    // Luma conversion (use tone-mapped values for edge detection)
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

    // Sample along edge (in HDR space)
    float3 rgbA = 0.5 * (
        screenTexture.Sample(linearSampler, texCoord + dir * (1.0 / 3.0 - 0.5)).rgb +
        screenTexture.Sample(linearSampler, texCoord + dir * (2.0 / 3.0 - 0.5)).rgb);

    float3 rgbB = rgbA * 0.5 + 0.25 * (
        screenTexture.Sample(linearSampler, texCoord + dir * -0.5).rgb +
        screenTexture.Sample(linearSampler, texCoord + dir * 0.5).rgb);

    float lumaB = dot(rgbB, luma);

    // Choose based on luma comparison
    float3 result;
    if (lumaB < lumaMin || lumaB > lumaMax)
        result = rgbA;
    else
        result = rgbB;

    // Apply SSAO (multiplied before tone mapping, only when enabled)
    if (uSSAOEnabled != 0)
    {
        float ao = ssaoTexture.Sample(linearSampler, texCoord).r;
        result *= ao;
    }

    // Apply exposure and ACES tone mapping
    result = ACESFilm(result * uExposure);

    // Gamma correction (linear -> sRGB)
    result = pow(result, 1.0 / 2.2);

    return float4(result, 1.0);
}
