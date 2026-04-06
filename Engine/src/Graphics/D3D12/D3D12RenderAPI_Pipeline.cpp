#include "D3D12RenderAPI.hpp"
#include "Utils/Log.hpp"
#include "Utils/EnginePaths.hpp"

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

    D3D12_ROOT_PARAMETER rootParams[5] = {};

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

    // Static samplers
    D3D12_STATIC_SAMPLER_DESC staticSamplers[2] = {};

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

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters = 5;
    rsDesc.pParameters = rootParams;
    rsDesc.NumStaticSamplers = 2;
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
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };
    static D3D12_INPUT_ELEMENT_DESC posOnlyLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    if (hasNormalAndTexcoord)
    {
        desc.InputLayout = { basicLayout, 3 };
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
    desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    desc.SampleDesc.Count = 1;

    return desc;
}

bool D3D12RenderAPI::createPipelineStates()
{
    LOG_ENGINE_TRACE("[D3D12] Creating pipeline state objects...");
    // Basic lit (cull back)
    {
        auto desc = CreateBasePSODesc(m_rootSignature.Get(), m_basicVS, m_basicPS);
        HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(m_psoBasicLit.GetAddressOf()));
        if (FAILED(hr)) { LOG_ENGINE_ERROR("Failed to create PSO: BasicLit"); return false; }
    }

    // Basic lit (cull front)
    {
        auto desc = CreateBasePSODesc(m_rootSignature.Get(), m_basicVS, m_basicPS);
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
        HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(m_psoBasicLitCullFront.GetAddressOf()));
        if (FAILED(hr)) { LOG_ENGINE_ERROR("Failed to create PSO: BasicLitCullFront"); return false; }
    }

    // Basic lit (cull none)
    {
        auto desc = CreateBasePSODesc(m_rootSignature.Get(), m_basicVS, m_basicPS);
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(m_psoBasicLitCullNone.GetAddressOf()));
        if (FAILED(hr)) { LOG_ENGINE_ERROR("Failed to create PSO: BasicLitCullNone"); return false; }
    }

    // Basic lit alpha blend
    {
        auto desc = CreateBasePSODesc(m_rootSignature.Get(), m_basicVS, m_basicPS);
        auto& rt = desc.BlendState.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_ZERO;
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(m_psoBasicLitAlpha.GetAddressOf()));
        if (FAILED(hr)) { LOG_ENGINE_ERROR("Failed to create PSO: BasicLitAlpha"); return false; }
    }

    // Basic lit alpha (cull none)
    {
        auto desc = CreateBasePSODesc(m_rootSignature.Get(), m_basicVS, m_basicPS);
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        auto& rt = desc.BlendState.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_ZERO;
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(m_psoBasicLitAlphaCullNone.GetAddressOf()));
        if (FAILED(hr)) { LOG_ENGINE_ERROR("Failed to create PSO: BasicLitAlphaCullNone"); return false; }
    }

    // Basic lit additive
    {
        auto desc = CreateBasePSODesc(m_rootSignature.Get(), m_basicVS, m_basicPS);
        auto& rt = desc.BlendState.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        rt.DestBlend = D3D12_BLEND_ONE;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_ZERO;
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(m_psoBasicLitAdditive.GetAddressOf()));
        if (FAILED(hr)) { LOG_ENGINE_ERROR("Failed to create PSO: BasicLitAdditive"); return false; }
    }

    // Unlit (cull back)
    {
        auto desc = CreateBasePSODesc(m_rootSignature.Get(), m_unlitVS, m_unlitPS);
        HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(m_psoUnlit.GetAddressOf()));
        if (FAILED(hr)) { LOG_ENGINE_ERROR("Failed to create PSO: Unlit"); return false; }
    }

    // Unlit (cull none)
    {
        auto desc = CreateBasePSODesc(m_rootSignature.Get(), m_unlitVS, m_unlitPS);
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(m_psoUnlitCullNone.GetAddressOf()));
        if (FAILED(hr)) { LOG_ENGINE_ERROR("Failed to create PSO: UnlitCullNone"); return false; }
    }

    // Unlit alpha
    {
        auto desc = CreateBasePSODesc(m_rootSignature.Get(), m_unlitVS, m_unlitPS);
        auto& rt = desc.BlendState.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_ZERO;
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(m_psoUnlitAlpha.GetAddressOf()));
        if (FAILED(hr)) { LOG_ENGINE_ERROR("Failed to create PSO: UnlitAlpha"); return false; }
    }

    // Unlit alpha (cull none)
    {
        auto desc = CreateBasePSODesc(m_rootSignature.Get(), m_unlitVS, m_unlitPS);
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        auto& rt = desc.BlendState.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_ZERO;
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(m_psoUnlitAlphaCullNone.GetAddressOf()));
        if (FAILED(hr)) { LOG_ENGINE_ERROR("Failed to create PSO: UnlitAlphaCullNone"); return false; }
    }

    // Unlit additive
    {
        auto desc = CreateBasePSODesc(m_rootSignature.Get(), m_unlitVS, m_unlitPS);
        auto& rt = desc.BlendState.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        rt.DestBlend = D3D12_BLEND_ONE;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_ZERO;
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(m_psoUnlitAdditive.GetAddressOf()));
        if (FAILED(hr)) { LOG_ENGINE_ERROR("Failed to create PSO: UnlitAdditive"); return false; }
    }

    // Debug lines (unlit, line list, no cull)
    {
        auto desc = CreateBasePSODesc(m_rootSignature.Get(), m_unlitVS, m_unlitPS);
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(m_psoDebugLines.GetAddressOf()));
        if (FAILED(hr)) { LOG_ENGINE_ERROR("Failed to create PSO: DebugLines"); return false; }
    }

    // Shadow (depth-only, cull front with depth bias)
    {
        auto desc = CreateBasePSODesc(m_rootSignature.Get(), m_shadowVS, m_shadowPS);
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
        desc.RasterizerState.DepthBias = 100;
        desc.RasterizerState.DepthBiasClamp = 0.0f;
        desc.RasterizerState.SlopeScaledDepthBias = 1.5f;
        desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        desc.NumRenderTargets = 0;
        desc.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
        desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(m_psoShadow.GetAddressOf()));
        if (FAILED(hr)) { LOG_ENGINE_ERROR("Failed to create PSO: Shadow"); return false; }
    }

    // Sky (depth read-only, cull back)
    {
        auto desc = CreateBasePSODesc(m_rootSignature.Get(), m_skyVS, m_skyPS, false);
        desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(m_psoSky.GetAddressOf()));
        if (FAILED(hr)) { LOG_ENGINE_ERROR("Failed to create PSO: Sky"); return false; }
    }

    // FXAA (no depth, fullscreen quad)
    {
        static D3D12_INPUT_ELEMENT_DESC fxaaLayout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
        };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = m_rootSignature.Get();
        desc.VS = { m_fxaaVS.data(), m_fxaaVS.size() };
        desc.PS = { m_fxaaPS.data(), m_fxaaPS.size() };
        desc.InputLayout = { fxaaLayout, 2 };
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.RasterizerState.DepthClipEnable = FALSE;
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.DepthStencilState.DepthEnable = FALSE;
        desc.SampleMask = UINT_MAX;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.DSVFormat = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;

        HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(m_psoFXAA.GetAddressOf()));
        if (FAILED(hr)) { LOG_ENGINE_ERROR("Failed to create PSO: FXAA"); return false; }
    }

    // Depth prepass (depth-only, no color output)
    {
        auto desc = CreateBasePSODesc(m_rootSignature.Get(), m_basicVS, {}); // No PS
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask = 0;
        desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(m_psoDepthPrepass.GetAddressOf()));
        if (FAILED(hr)) { LOG_ENGINE_ERROR("Failed to create PSO: DepthPrepass"); return false; }
    }

    LOG_ENGINE_TRACE("[D3D12] Created 15 pipeline state objects");
    return true;
}

// ============================================================================
// PSO Selection
// ============================================================================

ID3D12PipelineState* D3D12RenderAPI::selectPSO(const RenderState& state, bool unlit)
{
    if (in_depth_prepass)
        return m_psoDepthPrepass.Get();

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
