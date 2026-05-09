// GBuffer geometry pass for deferred rendering.
// Writes three render targets (UE4-style layout):
//   RT0.rgb = BaseColor,   RT0.a = Metallic        (RGBA8_UNORM)
//   RT1.rgb = WorldNormal, RT1.a = Roughness       (RGBA16_FLOAT, normal stored in [-1,1])
//   RT2.rgb = Emissive,    RT2.a = AmbientOcclusion (RGBA16_FLOAT)
// Depth is written to the existing scene depth buffer.
//
// Bindings mirror basic.slang so the same per-draw descriptor tables work
// (GlobalCB at b0, PerObjectCB at b1, PBR textures at t0..t5). No light CB
// is bound here — lighting is applied later by deferred_lighting.slang.

[[vk::binding(0, 0)]]
cbuffer GlobalCB : register(b0)
{
    float4x4 uView;
    float4x4 uProjection;
    float4x4 uLightSpaceMatrices[4];
    float4 uCascadeSplits;
    float3 uLightDir;
    float uCascadeSplit4;
    float3 uLightAmbient;
    int uCascadeCount;
    float3 uLightDiffuse;
    int uDebugCascades;
    float2 uShadowMapTexelSize;
    float2 _shadowPad;
};

[[vk::binding(4, 0)]]
cbuffer PerObjectCB : register(b1)
{
    float4x4 uModel;
    float4x4 uNormalMatrix;
    float3 uColor;
    int uUseTexture;
    float uAlphaCutoff;
    float uMetallic;
    float uRoughness;
    float _pbrPad1;
    float3 uEmissive;
    float _pbrPad2;
    int uHasMetallicRoughnessMap;
    int uHasNormalMap;
    int uHasOcclusionMap;
    int uHasEmissiveMap;
    int uUseHeightmapDisplacement;
    float uHeightmapScale;
    float uHeightmapOffset;
    float _heightmapPad0;
    float2 uHeightmapTexelSize;
    float2 _heightmapPad1;
    int uMaterialFlags;
    float3 _materialPad0;
#ifdef TARGET_SPIRV
    int uUseInstanceData;
    uint uInstanceBase;
    float2 _instancePad;
#endif
};

[[vk::binding(1, 0)]]Texture2D diffuseTexture : register(t0);
[[vk::binding(1, 0)]]SamplerState linearSampler : register(s0);

[[vk::binding(6, 0)]]Texture2D metallicRoughnessTexture : register(t2);
[[vk::binding(6, 0)]]SamplerState metallicRoughnessSampler : register(s2);

[[vk::binding(7, 0)]]Texture2D normalTexture : register(t3);
[[vk::binding(7, 0)]]SamplerState normalSampler : register(s3);

[[vk::binding(8, 0)]]Texture2D occlusionTexture : register(t4);
[[vk::binding(8, 0)]]SamplerState occlusionSampler : register(s4);

[[vk::binding(9, 0)]]Texture2D emissiveTexture : register(t5);
[[vk::binding(9, 0)]]SamplerState emissiveSampler : register(s5);

[[vk::binding(13, 0)]]Texture2D heightmapTexture : register(t8);

#ifdef TARGET_SPIRV
struct InstanceData
{
    float4x4 model;
    float4x4 normalMatrix;
};
[[vk::binding(12, 0)]]StructuredBuffer<InstanceData> instanceData : register(t9);
#endif

struct VSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float2 texcoord : TEXCOORD0;
    float4 tangent  : TANGENT;
};

struct PSInput
{
    float4 position  : SV_POSITION;
    float3 fragPos   : TEXCOORD0;
    float3 normal    : TEXCOORD1;
    float2 texcoord  : TEXCOORD2;
    float3 tangent   : TEXCOORD3;
    float3 bitangent : TEXCOORD4;
};

struct PSOutput
{
    float4 RT0 : SV_Target0; // BaseColor + Metallic
    float4 RT1 : SV_Target1; // WorldNormal + Roughness
    float4 RT2 : SV_Target2; // Emissive + AO
};

float sampleHeightmap(float2 uv)
{
    float2 texelSize = max(uHeightmapTexelSize, float2(0.000001, 0.000001));
    float2 dimensions = max(round(1.0 / texelSize), float2(1.0, 1.0));
    int2 coord = int2(round(saturate(uv) * (dimensions - 1.0)));
    return heightmapTexture.Load(int3(coord, 0)).r * uHeightmapScale + uHeightmapOffset;
}

float3 applyHeightmapDisplacement(float3 position, float2 uv)
{
    if (uUseHeightmapDisplacement != 0)
        position.y += sampleHeightmap(uv);
    return position;
}

[shader("vertex")]
#ifdef TARGET_SPIRV
PSInput vertexMain(VSInput input, uint instanceId : SV_InstanceID)
#else
PSInput vertexMain(VSInput input)
#endif
{
    PSInput output;

#ifdef TARGET_SPIRV
    float4x4 model = uModel;
    float4x4 normalMatrix = uNormalMatrix;
    if (uUseInstanceData != 0)
    {
        InstanceData inst = instanceData[uInstanceBase + instanceId];
        model = inst.model;
        normalMatrix = inst.normalMatrix;
    }
#else
    float4x4 model = uModel;
    float4x4 normalMatrix = uNormalMatrix;
#endif

    float3 localPosition = applyHeightmapDisplacement(input.position, input.texcoord);
    float4 worldPos = mul(model, float4(localPosition, 1.0));
    output.fragPos = worldPos.xyz;

    float3x3 normalMat = (float3x3)normalMatrix;
    output.normal = normalize(mul(normalMat, input.normal));
    output.tangent = normalize(mul(normalMat, input.tangent.xyz));
    output.bitangent = cross(output.normal, output.tangent) * input.tangent.w;

    output.texcoord = input.texcoord;
    output.position = mul(uProjection, mul(uView, worldPos));
    return output;
}

[shader("fragment")]
PSOutput fragmentMain(PSInput input)
{
    float3 T = normalize(input.tangent);
    float3 B = normalize(input.bitangent);
    float3 N = normalize(input.normal);

    if (uHasNormalMap != 0)
    {
        float3 normalSample = normalTexture.Sample(normalSampler, input.texcoord).rgb;
        normalSample = normalSample * 2.0 - 1.0;
        float3x3 TBN = float3x3(T, B, N);
        N = normalize(mul(normalSample, TBN));
    }

    float4 texColor;
    if (uUseTexture != 0)
    {
        texColor = diffuseTexture.Sample(linearSampler, input.texcoord);
        if (uAlphaCutoff > 0.0 && texColor.a < uAlphaCutoff)
            discard;
    }
    else
    {
        texColor = float4(uColor, 1.0);
    }

    float3 albedo = texColor.rgb;
    float metallic = uMetallic;
    float roughness = uRoughness;

    if (uHasMetallicRoughnessMap != 0)
    {
        float4 mrSample = metallicRoughnessTexture.Sample(metallicRoughnessSampler, input.texcoord);
        roughness = mrSample.g * uRoughness;
        metallic = mrSample.b * uMetallic;
    }
    roughness = max(roughness, 0.04);

    float ao = 1.0;
    if (uHasOcclusionMap != 0)
    {
        ao = occlusionTexture.Sample(occlusionSampler, input.texcoord).r;
    }

    float3 emissive = uEmissive;
    if (uHasEmissiveMap != 0)
    {
        emissive *= emissiveTexture.Sample(emissiveSampler, input.texcoord).rgb;
    }

    PSOutput output;
    output.RT0 = float4(albedo, metallic);
    output.RT1 = float4(N, roughness);
    output.RT2 = float4(emissive, ao);
    return output;
}
