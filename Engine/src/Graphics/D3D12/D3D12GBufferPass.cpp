#include "D3D12GBufferPass.hpp"
#include "Utils/Log.hpp"

D3D12GBufferPass::~D3D12GBufferPass()
{
    cleanup();
}

bool D3D12GBufferPass::init(ID3D12Device* device,
                            D3D12PSOCache& psoCache,
                            ID3D12RootSignature* sharedRootSig,
                            const std::vector<char>& gbufferVS,
                            const std::vector<char>& gbufferPS)
{
    if (!device || !sharedRootSig || gbufferVS.empty() || gbufferPS.empty())
        return false;

    static const D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TANGENT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = sharedRootSig;
    desc.VS = { gbufferVS.data(), gbufferVS.size() };
    desc.PS = { gbufferPS.data(), gbufferPS.size() };
    desc.InputLayout = { inputLayout, _countof(inputLayout) };

    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    desc.RasterizerState.FrontCounterClockwise = TRUE;
    desc.RasterizerState.DepthClipEnable = TRUE;

    for (int i = 0; i < 3; ++i)
        desc.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    desc.DepthStencilState.DepthEnable = TRUE;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;

    desc.SampleMask = UINT_MAX;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 3;
    desc.RTVFormats[0] = RT0_FORMAT;
    desc.RTVFormats[1] = RT1_FORMAT;
    desc.RTVFormats[2] = RT2_FORMAT;
    desc.DSVFormat = DSV_FORMAT;
    desc.SampleDesc.Count = 1;

    pso_ = psoCache.loadGraphicsPSO(L"GBuffer", desc);
    if (!pso_) {
        HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(pso_.GetAddressOf()));
        if (FAILED(hr)) {
            LOG_ENGINE_ERROR("[D3D12] Failed to create GBuffer PSO (hr={})", (unsigned)hr);
            return false;
        }
        psoCache.storePSO(L"GBuffer", pso_.Get());
    }

    initialized_ = true;
    return true;
}

void D3D12GBufferPass::cleanup()
{
    pso_.Reset();
    initialized_ = false;
}
