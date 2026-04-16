#include "D3D12RenderAPI.hpp"
#include "Utils/Log.hpp"
#include "Utils/EnginePaths.hpp"
#include <filesystem>

// ============================================================================
// Root Signature
// ============================================================================

bool D3D12RenderAPI::createRootSignature()
{
    LOG_ENGINE_TRACE("[D3D12] Creating root signature...");
    // Root parameters:
    // [0] Root CBV b0 - GlobalCBuffer / ShadowCBuffer / SkyboxCBuffer / FXAACBuffer
    // [1] Root CBV b1 - PerObjectCBuffer
    // [2] Descriptor table: SRV t0 (diffuse texture)
    // [3] Descriptor table: SRV t1 (shadow map)
    // [4] Root CBV b3 - LightCBuffer
    // [5] Descriptor table: SRV t2 (metallic-roughness texture)
    // [6] Descriptor table: SRV t3 (normal map texture)
    // [7] Descriptor table: SRV t4 (occlusion texture)
    // [8] Descriptor table: SRV t5 (emissive texture)

    D3D12_ROOT_PARAMETER rootParams[9] = {};

    // [0] Root CBV b0
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[0].Descriptor.ShaderRegister = 0;
    rootParams[0].Descriptor.RegisterSpace = 0;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // [1] Root CBV b1
    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[1].Descriptor.ShaderRegister = 1;
    rootParams[1].Descriptor.RegisterSpace = 0;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // [2] Descriptor table: SRV t0 (diffuse texture)
    D3D12_DESCRIPTOR_RANGE srvRange0 = {};
    srvRange0.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange0.NumDescriptors = 1;
    srvRange0.BaseShaderRegister = 0;
    srvRange0.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    rootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[2].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[2].DescriptorTable.pDescriptorRanges = &srvRange0;
    rootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // [3] Descriptor table: SRV t1 (shadow map)
    D3D12_DESCRIPTOR_RANGE srvRange1 = {};
    srvRange1.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange1.NumDescriptors = 1;
    srvRange1.BaseShaderRegister = 1;
    srvRange1.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    rootParams[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[3].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[3].DescriptorTable.pDescriptorRanges = &srvRange1;
    rootParams[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // [4] Root CBV b3 (LightCBuffer)
    rootParams[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[4].Descriptor.ShaderRegister = 3;
    rootParams[4].Descriptor.RegisterSpace = 0;
    rootParams[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // [5] Descriptor table: SRV t2 (metallic-roughness texture)
    D3D12_DESCRIPTOR_RANGE srvRange2 = {};
    srvRange2.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange2.NumDescriptors = 1;
    srvRange2.BaseShaderRegister = 2;
    srvRange2.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    rootParams[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[5].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[5].DescriptorTable.pDescriptorRanges = &srvRange2;
    rootParams[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // [6] Descriptor table: SRV t3 (normal map texture)
    D3D12_DESCRIPTOR_RANGE srvRange3 = {};
    srvRange3.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange3.NumDescriptors = 1;
    srvRange3.BaseShaderRegister = 3;
    srvRange3.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    rootParams[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[6].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[6].DescriptorTable.pDescriptorRanges = &srvRange3;
    rootParams[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // [7] Descriptor table: SRV t4 (occlusion texture)
    D3D12_DESCRIPTOR_RANGE srvRange4 = {};
    srvRange4.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange4.NumDescriptors = 1;
    srvRange4.BaseShaderRegister = 4;
    srvRange4.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    rootParams[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[7].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[7].DescriptorTable.pDescriptorRanges = &srvRange4;
    rootParams[7].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // [8] Descriptor table: SRV t5 (emissive texture)
    D3D12_DESCRIPTOR_RANGE srvRange5 = {};
    srvRange5.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange5.NumDescriptors = 1;
    srvRange5.BaseShaderRegister = 5;
    srvRange5.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    rootParams[8].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[8].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[8].DescriptorTable.pDescriptorRanges = &srvRange5;
    rootParams[8].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // Static samplers
    D3D12_STATIC_SAMPLER_DESC staticSamplers[6] = {};

    // s0: Anisotropic wrap (diffuse textures)
    staticSamplers[0].Filter = D3D12_FILTER_ANISOTROPIC;
    staticSamplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSamplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSamplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSamplers[0].MaxAnisotropy = 16;
    staticSamplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    staticSamplers[0].MaxLOD = D3D12_FLOAT32_MAX;
    staticSamplers[0].ShaderRegister = 0;
    staticSamplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // s1: Shadow comparison sampler
    staticSamplers[1].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    staticSamplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    staticSamplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    staticSamplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    staticSamplers[1].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    staticSamplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    staticSamplers[1].MaxLOD = D3D12_FLOAT32_MAX;
    staticSamplers[1].ShaderRegister = 1;
    staticSamplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // s2-s5: Anisotropic wrap samplers for PBR textures (metallic-roughness, normal, occlusion, emissive)
    for (int i = 2; i <= 5; i++)
    {
        staticSamplers[i].Filter = D3D12_FILTER_ANISOTROPIC;
        staticSamplers[i].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        staticSamplers[i].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        staticSamplers[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        staticSamplers[i].MaxAnisotropy = 16;
        staticSamplers[i].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        staticSamplers[i].MaxLOD = D3D12_FLOAT32_MAX;
        staticSamplers[i].ShaderRegister = i;
        staticSamplers[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters = 9;
    rsDesc.pParameters = rootParams;
    rsDesc.NumStaticSamplers = 6;
    rsDesc.pStaticSamplers = staticSamplers;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> error;
    HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                              serialized.GetAddressOf(), error.GetAddressOf());
    if (FAILED(hr))
    {
        if (error)
            LOG_ENGINE_ERROR("Root signature serialization failed: {}", static_cast<char*>(error->GetBufferPointer()));
        return false;
    }

    hr = device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                      IID_PPV_ARGS(m_rootSignature.GetAddressOf()));
    return SUCCEEDED(hr);
}

// ============================================================================
// Shaders and Pipeline States
// ============================================================================

bool D3D12RenderAPI::loadShaders()
{
    LOG_ENGINE_TRACE("[D3D12] Loading DXIL shaders...");
    std::string shaderDir = EnginePaths::resolveEngineAsset("../assets/shaders/compiled/d3d12/");

    m_basicVS = readShaderBinary(shaderDir + "basic_vs.dxil");
    if (m_basicVS.empty()) return false;
    m_basicPS = readShaderBinary(shaderDir + "basic_ps.dxil");
    if (m_basicPS.empty()) return false;

    m_unlitVS = readShaderBinary(shaderDir + "unlit_vs.dxil");
    if (m_unlitVS.empty()) return false;
    m_unlitPS = readShaderBinary(shaderDir + "unlit_ps.dxil");
    if (m_unlitPS.empty()) return false;

    m_shadowVS = readShaderBinary(shaderDir + "shadow_vs.dxil");
    if (m_shadowVS.empty()) return false;
    m_shadowPS = readShaderBinary(shaderDir + "shadow_ps.dxil");
    // Shadow PS may be empty (depth-only), which is fine

    m_shadowAlphaTestVS = readShaderBinary(shaderDir + "shadow_alphatest_vs.dxil");
    m_shadowAlphaTestPS = readShaderBinary(shaderDir + "shadow_alphatest_ps.dxil");
    // Shadow alpha-test shaders are optional — fall back to opaque shadow if missing

    m_skyVS = readShaderBinary(shaderDir + "sky_vs.dxil");
    if (m_skyVS.empty()) return false;
    m_skyPS = readShaderBinary(shaderDir + "sky_ps.dxil");
    if (m_skyPS.empty()) return false;

    m_fxaaVS = readShaderBinary(shaderDir + "fxaa_vs.dxil");
    if (m_fxaaVS.empty()) return false;
    m_fxaaPS = readShaderBinary(shaderDir + "fxaa_ps.dxil");
    if (m_fxaaPS.empty()) return false;

    LOG_ENGINE_TRACE("[D3D12] Loaded 10 DXIL shaders (basic, unlit, shadow, sky, fxaa)");
    return true;
}

static D3D12_GRAPHICS_PIPELINE_STATE_DESC CreateBasePSODesc(
    ID3D12RootSignature* rootSig,
    const std::vector<char>& vs, const std::vector<char>& ps,
    bool hasNormalAndTexcoord = true)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = rootSig;
    desc.VS = { vs.data(), vs.size() };
    if (!ps.empty())
        desc.PS = { ps.data(), ps.size() };

    // Input layout
    static D3D12_INPUT_ELEMENT_DESC basicLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TANGENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };
    static D3D12_INPUT_ELEMENT_DESC posOnlyLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    if (hasNormalAndTexcoord)
    {
        desc.InputLayout = { basicLayout, 4 };
    }
    else
    {
        desc.InputLayout = { posOnlyLayout, 1 };
    }

    // Rasterizer defaults
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    desc.RasterizerState.FrontCounterClockwise = TRUE;
    desc.RasterizerState.DepthClipEnable = TRUE;

    // Blend defaults (no blend)
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    // Depth defaults
    desc.DepthStencilState.DepthEnable = TRUE;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;

    desc.SampleMask = UINT_MAX;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    desc.SampleDesc.Count = 1;

    return desc;
}

// Helper: try loading a PSO from cache, fall back to creation, then store in cache.
static ComPtr<ID3D12PipelineState> CreateOrLoadPSO(
    ID3D12Device* device, D3D12PSOCache& cache,
    const wchar_t* name, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc)
{
    auto pso = cache.loadGraphicsPSO(name, desc);
    if (pso) return pso;

    HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(pso.GetAddressOf()));
    if (FAILED(hr)) return nullptr;

    cache.storePSO(name, pso.Get());
    return pso;
}

bool D3D12RenderAPI::createPipelineStates()
{
    LOG_ENGINE_TRACE("[D3D12] Creating pipeline state objects...");

    // Initialize PSO cache
    std::string cacheDir = EnginePaths::resolveEngineAsset("../cache/");
    std::filesystem::create_directories(cacheDir);
    m_psoCachePath = cacheDir + "d3d12_pso_cache.bin";
    m_psoCache.loadFromDisk(device.Get(), m_psoCachePath);

    int cached = 0, compiled = 0;

    // Helper lambda to track cache hits
    auto createPSO = [&](const wchar_t* name, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc) -> ComPtr<ID3D12PipelineState>
    {
        auto pso = m_psoCache.loadGraphicsPSO(name, desc);
        if (pso) { cached++; return pso; }
        HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(pso.GetAddressOf()));
        if (FAILED(hr)) return nullptr;
        m_psoCache.storePSO(name, pso.Get());
        compiled++;
        return pso;
    };

    // Basic lit (cull back)
    {
        auto desc = CreateBasePSODesc(m_rootSignature.Get(), m_basicVS, m_basicPS);
        m_psoBasicLit = createPSO(L"BasicLit", desc);
        if (!m_psoBasicLit) { LOG_ENGINE_ERROR("Failed to create PSO: BasicLit"); return false; }
    }

    // Basic lit (cull front)
    {
        auto desc = CreateBasePSODesc(m_rootSignature.Get(), m_basicVS, m_basicPS);
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
        m_psoBasicLitCullFront = createPSO(L"BasicLitCullFront", desc);
        if (!m_psoBasicLitCullFront) { LOG_ENGINE_ERROR("Failed to create PSO: BasicLitCullFront"); return false; }
    }

    // Basic lit (cull none)
    {
        auto desc = CreateBasePSODesc(m_rootSignature.Get(), m_basicVS, m_basicPS);
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        m_psoBasicLitCullNone = createPSO(L"BasicLitCullNone", desc);
        if (!m_psoBasicLitCullNone) { LOG_ENGINE_ERROR("Failed to create PSO: BasicLitCullNone"); return false; }
    }

    // Basic lit alpha blend
    {
        auto desc = CreateBasePSODesc(m_rootSignature.Get(), m_basicVS, m_basicPS);
        desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        auto& rt = desc.BlendState.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_ZERO;
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        m_psoBasicLitAlpha = createPSO(L"BasicLitAlpha", desc);
        if (!m_psoBasicLitAlpha) { LOG_ENGINE_ERROR("Failed to create PSO: BasicLitAlpha"); return false; }
    }

    // Basic lit alpha (cull none)
    {
        auto desc = CreateBasePSODesc(m_rootSignature.Get(), m_basicVS, m_basicPS);
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        auto& rt = desc.BlendState.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_ZERO;
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        m_psoBasicLitAlphaCullNone = createPSO(L"BasicLitAlphaCullNone", desc);
        if (!m_psoBasicLitAlphaCullNone) { LOG_ENGINE_ERROR("Failed to create PSO: BasicLitAlphaCullNone"); return false; }
    }

    // Basic lit additive
    {
        auto desc = CreateBasePSODesc(m_rootSignature.Get(), m_basicVS, m_basicPS);
        desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        auto& rt = desc.BlendState.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        rt.DestBlend = D3D12_BLEND_ONE;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_ZERO;
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        m_psoBasicLitAdditive = createPSO(L"BasicLitAdditive", desc);
        if (!m_psoBasicLitAdditive) { LOG_ENGINE_ERROR("Failed to create PSO: BasicLitAdditive"); return false; }
    }

    // Unlit (cull back)
    {
        auto desc = CreateBasePSODesc(m_rootSignature.Get(), m_unlitVS, m_unlitPS);
        m_psoUnlit = createPSO(L"Unlit", desc);
        if (!m_psoUnlit) { LOG_ENGINE_ERROR("Failed to create PSO: Unlit"); return false; }
    }

    // Unlit (cull none)
    {
        auto desc = CreateBasePSODesc(m_rootSignature.Get(), m_unlitVS, m_unlitPS);
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        m_psoUnlitCullNone = createPSO(L"UnlitCullNone", desc);
        if (!m_psoUnlitCullNone) { LOG_ENGINE_ERROR("Failed to create PSO: UnlitCullNone"); return false; }
    }

    // Unlit alpha
    {
        auto desc = CreateBasePSODesc(m_rootSignature.Get(), m_unlitVS, m_unlitPS);
        desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        auto& rt = desc.BlendState.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_ZERO;
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        m_psoUnlitAlpha = createPSO(L"UnlitAlpha", desc);
        if (!m_psoUnlitAlpha) { LOG_ENGINE_ERROR("Failed to create PSO: UnlitAlpha"); return false; }
    }

    // Unlit alpha (cull none)
    {
        auto desc = CreateBasePSODesc(m_rootSignature.Get(), m_unlitVS, m_unlitPS);
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        auto& rt = desc.BlendState.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_ZERO;
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        m_psoUnlitAlphaCullNone = createPSO(L"UnlitAlphaCullNone", desc);
        if (!m_psoUnlitAlphaCullNone) { LOG_ENGINE_ERROR("Failed to create PSO: UnlitAlphaCullNone"); return false; }
    }

    // Unlit additive
    {
        auto desc = CreateBasePSODesc(m_rootSignature.Get(), m_unlitVS, m_unlitPS);
        desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        auto& rt = desc.BlendState.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        rt.DestBlend = D3D12_BLEND_ONE;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_ZERO;
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        m_psoUnlitAdditive = createPSO(L"UnlitAdditive", desc);
        if (!m_psoUnlitAdditive) { LOG_ENGINE_ERROR("Failed to create PSO: UnlitAdditive"); return false; }
    }

    // Debug lines (unlit, line list, no cull)
    {
        auto desc = CreateBasePSODesc(m_rootSignature.Get(), m_unlitVS, m_unlitPS);
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        m_psoDebugLines = createPSO(L"DebugLines", desc);
        if (!m_psoDebugLines) { LOG_ENGINE_ERROR("Failed to create PSO: DebugLines"); return false; }
    }

    // Shadow (depth-only, cull front with depth bias)
    {
        auto desc = CreateBasePSODesc(m_rootSignature.Get(), m_shadowVS, m_shadowPS);
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.RasterizerState.DepthBias = 1000;
        desc.RasterizerState.DepthBiasClamp = 0.0f;
        desc.RasterizerState.SlopeScaledDepthBias = 1.0f;
        desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        desc.NumRenderTargets = 0;
        desc.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
        desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        m_psoShadow = createPSO(L"Shadow", desc);
        if (!m_psoShadow) { LOG_ENGINE_ERROR("Failed to create PSO: Shadow"); return false; }
    }

    // Sky PSO is now created inside D3D12PostProcessPass (m_skyPass)
    // FXAA PSO is now created inside D3D12PostProcessPass (m_fxaaPass)

    // Depth prepass (depth-only, no color output)
    {
        auto desc = CreateBasePSODesc(m_rootSignature.Get(), m_basicVS, {}); // No PS
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask = 0;
        desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        m_psoDepthPrepass = createPSO(L"DepthPrepass", desc);
        if (!m_psoDepthPrepass) { LOG_ENGINE_ERROR("Failed to create PSO: DepthPrepass"); return false; }
    }

    // Depth prepass with alpha test (uses basic PS for discard, color writes off)
    {
        auto desc = CreateBasePSODesc(m_rootSignature.Get(), m_basicVS, m_basicPS);
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask = 0;
        desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        m_psoDepthPrepassAlphaTest = createPSO(L"DepthPrepassAlphaTest", desc);
        if (!m_psoDepthPrepassAlphaTest) { LOG_ENGINE_ERROR("Failed to create PSO: DepthPrepassAlphaTest"); return false; }
    }
    {
        auto desc = CreateBasePSODesc(m_rootSignature.Get(), m_basicVS, m_basicPS);
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask = 0;
        desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        m_psoDepthPrepassAlphaTestCullNone = createPSO(L"DepthPrepassAlphaTestCullNone", desc);
        if (!m_psoDepthPrepassAlphaTestCullNone) { LOG_ENGINE_ERROR("Failed to create PSO: DepthPrepassAlphaTestCullNone"); return false; }
    }

    // Shadow with alpha test (uses shadow_alphatest shader for texture sampling + discard)
    if (!m_shadowAlphaTestVS.empty() && !m_shadowAlphaTestPS.empty())
    {
        auto desc = CreateBasePSODesc(m_rootSignature.Get(), m_shadowAlphaTestVS, m_shadowAlphaTestPS);
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.RasterizerState.DepthBias = 1000;
        desc.RasterizerState.DepthBiasClamp = 0.0f;
        desc.RasterizerState.SlopeScaledDepthBias = 1.0f;
        desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        desc.NumRenderTargets = 0;
        desc.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
        desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        m_psoShadowAlphaTest = createPSO(L"ShadowAlphaTest", desc);
        if (!m_psoShadowAlphaTest) { LOG_ENGINE_WARN("Failed to create PSO: ShadowAlphaTest — alpha-masked shadows disabled"); }
    }

    LOG_ENGINE_INFO("[D3D12] Pipeline states: {} cached, {} compiled", cached, compiled);
    return true;
}

// ============================================================================
// PSO Selection
// ============================================================================

ID3D12PipelineState* D3D12RenderAPI::selectPSO(const RenderState& state, bool unlit)
{
    if (in_depth_prepass)
    {
        if (state.alpha_test)
            return (state.cull_mode == CullMode::None)
                ? m_psoDepthPrepassAlphaTestCullNone.Get()
                : m_psoDepthPrepassAlphaTest.Get();
        return m_psoDepthPrepass.Get();
    }

    if (unlit)
    {
        switch (state.blend_mode)
        {
        case BlendMode::Alpha:
            return (state.cull_mode == CullMode::None) ? m_psoUnlitAlphaCullNone.Get() : m_psoUnlitAlpha.Get();
        case BlendMode::Additive:
            return m_psoUnlitAdditive.Get();
        default:
            return (state.cull_mode == CullMode::None) ? m_psoUnlitCullNone.Get() : m_psoUnlit.Get();
        }
    }

    switch (state.blend_mode)
    {
    case BlendMode::Alpha:
        return (state.cull_mode == CullMode::None) ? m_psoBasicLitAlphaCullNone.Get() : m_psoBasicLitAlpha.Get();
    case BlendMode::Additive:
        return m_psoBasicLitAdditive.Get();
    default:
        switch (state.cull_mode)
        {
        case CullMode::Front: return m_psoBasicLitCullFront.Get();
        case CullMode::None:  return m_psoBasicLitCullNone.Get();
        default:              return m_psoBasicLit.Get();
        }
    }
}
