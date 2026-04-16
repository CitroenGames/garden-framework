#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "D3D12PostProcessPass.hpp"
#include "Utils/Log.hpp"
#include <algorithm>
#include <cstring>

// ============================================================================
// Lifecycle
// ============================================================================

D3D12PostProcessPass::~D3D12PostProcessPass()
{
    cleanup();
}

D3D12PostProcessPass::D3D12PostProcessPass(D3D12PostProcessPass&& o) noexcept
{
    *this = std::move(o);
}

D3D12PostProcessPass& D3D12PostProcessPass::operator=(D3D12PostProcessPass&& o) noexcept
{
    if (this == &o) return *this;
    cleanup();

    device_       = o.device_;
    rtvAllocator_ = o.rtvAllocator_;
    srvAllocator_ = o.srvAllocator_;
    stateTracker_ = o.stateTracker_;
    config_       = std::move(o.config_);
    initialized_  = o.initialized_;

    rootSignature_ = std::move(o.rootSignature_);
    pso_           = std::move(o.pso_);

    outputTexture_  = std::move(o.outputTexture_);
    outputRTVIndex_ = o.outputRTVIndex_;
    outputSRVIndex_ = o.outputSRVIndex_;

    width_  = o.width_;
    height_ = o.height_;

    // Null out moved-from object
    o.device_       = nullptr;
    o.rtvAllocator_ = nullptr;
    o.srvAllocator_ = nullptr;
    o.stateTracker_ = nullptr;
    o.initialized_  = false;
    o.outputRTVIndex_ = UINT(-1);
    o.outputSRVIndex_ = UINT(-1);
    o.width_  = 0;
    o.height_ = 0;

    return *this;
}

// ============================================================================
// init
// ============================================================================

bool D3D12PostProcessPass::init(
    ID3D12Device* device,
    DescriptorHeapAllocator& rtvAllocator,
    DescriptorHeapAllocator& srvAllocator,
    D3D12ResourceStateTracker& stateTracker,
    D3D12PSOCache& psoCache,
    const D3D12PostProcessPassConfig& config,
    uint32_t referenceWidth, uint32_t referenceHeight,
    const std::vector<char>& vs,
    const std::vector<char>& ps)
{
    device_       = device;
    rtvAllocator_ = &rtvAllocator;
    srvAllocator_ = &srvAllocator;
    stateTracker_ = &stateTracker;
    config_       = config;

    computeDimensions(referenceWidth, referenceHeight);

    // 1. Root signature
    if (!createRootSignature()) return false;

    // 2. PSO
    if (!createPSO(psoCache, vs, ps)) return false;

    // 3. Output texture (own-output mode)
    if (!config_.useExternalRTV) {
        if (!createOutputTexture()) return false;
    }

    initialized_ = true;

    // Convert wchar_t debug name to char for logging
    char debugNameA[128] = "PostProcess";
    if (config_.debugName) {
        WideCharToMultiByte(CP_UTF8, 0, config_.debugName, -1, debugNameA, 128, nullptr, nullptr);
    }
    LOG_ENGINE_INFO("[D3D12] {} pass created ({}x{})", debugNameA, width_, height_);
    return true;
}

// ============================================================================
// cleanup
// ============================================================================

void D3D12PostProcessPass::cleanup()
{
    if (!device_) return;
    initialized_ = false;

    destroyOutputTexture();

    pso_.Reset();
    rootSignature_.Reset();

    device_ = nullptr;
}

// ============================================================================
// resize
// ============================================================================

void D3D12PostProcessPass::resize(uint32_t newWidth, uint32_t newHeight)
{
    if (!device_) return;

    computeDimensions(newWidth, newHeight);

    // Recreate output texture at new size (own-output mode only)
    if (!config_.useExternalRTV) {
        destroyOutputTexture();
        createOutputTexture();
    }
}

// ============================================================================
// Recording
// ============================================================================

void D3D12PostProcessPass::begin(
    ID3D12GraphicsCommandList* cmd,
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle,
    uint32_t width, uint32_t height)
{
    if (!initialized_) return;

    // Set render target (no depth)
    cmd->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

    // Viewport
    D3D12_VIEWPORT vp = {};
    vp.Width    = static_cast<float>(width);
    vp.Height   = static_cast<float>(height);
    vp.MaxDepth = 1.0f;
    cmd->RSSetViewports(1, &vp);

    // Scissor
    D3D12_RECT scissor = { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
    cmd->RSSetScissorRects(1, &scissor);

    // Root signature + PSO
    cmd->SetGraphicsRootSignature(rootSignature_.Get());
    cmd->SetPipelineState(pso_.Get());

    // Topology (fullscreen quad = triangle strip with 4 vertices)
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
}

void D3D12PostProcessPass::begin(ID3D12GraphicsCommandList* cmd)
{
    if (!initialized_ || config_.useExternalRTV) return;

    // Transition output to render target and flush the barrier
    stateTracker_->transition(outputTexture_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
    stateTracker_->flush(cmd);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rtvAllocator_->getCPU(outputRTVIndex_);
    begin(cmd, rtvHandle, width_, height_);
}

void D3D12PostProcessPass::draw(
    ID3D12GraphicsCommandList* cmd,
    const D3D12_VERTEX_BUFFER_VIEW& quadVBV)
{
    if (!initialized_) return;

    cmd->IASetVertexBuffers(0, 1, &quadVBV);
    cmd->DrawInstanced(4, 1, 0, 0);
}

// ============================================================================
// Private helpers
// ============================================================================

void D3D12PostProcessPass::computeDimensions(uint32_t refWidth, uint32_t refHeight)
{
    width_  = std::max(1u, static_cast<uint32_t>(refWidth  * config_.scaleFactor));
    height_ = std::max(1u, static_cast<uint32_t>(refHeight * config_.scaleFactor));
}

bool D3D12PostProcessPass::createRootSignature()
{
    // Build root parameters from config bindings
    std::vector<D3D12_ROOT_PARAMETER> rootParams(config_.bindings.size());
    std::vector<D3D12_DESCRIPTOR_RANGE> srvRanges(config_.bindings.size()); // storage for SRV ranges

    for (size_t i = 0; i < config_.bindings.size(); i++) {
        const auto& b = config_.bindings[i];

        if (b.type == D3D12PPBinding::CBV) {
            rootParams[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            rootParams[i].Descriptor.ShaderRegister = b.shaderRegister;
            rootParams[i].Descriptor.RegisterSpace = 0;
            rootParams[i].ShaderVisibility = b.visibility;
        }
        else { // SRV_TABLE
            srvRanges[i] = {};
            srvRanges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            srvRanges[i].NumDescriptors = 1;
            srvRanges[i].BaseShaderRegister = b.shaderRegister;
            srvRanges[i].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

            rootParams[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            rootParams[i].DescriptorTable.NumDescriptorRanges = 1;
            rootParams[i].DescriptorTable.pDescriptorRanges = &srvRanges[i];
            rootParams[i].ShaderVisibility = b.visibility;
        }
    }

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters = static_cast<UINT>(rootParams.size());
    rsDesc.pParameters = rootParams.data();
    rsDesc.NumStaticSamplers = static_cast<UINT>(config_.staticSamplers.size());
    rsDesc.pStaticSamplers = config_.staticSamplers.data();
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> error;
    HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                              serialized.GetAddressOf(), error.GetAddressOf());
    if (FAILED(hr)) {
        if (error)
            LOG_ENGINE_ERROR("[D3D12] PostProcess root signature serialization failed: {}",
                             static_cast<char*>(error->GetBufferPointer()));
        return false;
    }

    hr = device_->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                       IID_PPV_ARGS(rootSignature_.GetAddressOf()));
    if (FAILED(hr)) {
        LOG_ENGINE_ERROR("[D3D12] PostProcess root signature creation failed");
        return false;
    }

    return true;
}

bool D3D12PostProcessPass::createPSO(
    D3D12PSOCache& psoCache,
    const std::vector<char>& vs,
    const std::vector<char>& ps)
{
    // Fullscreen quad vertex input: vec2 pos + vec2 texCoord
    static D3D12_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = rootSignature_.Get();
    desc.VS = { vs.data(), vs.size() };
    desc.PS = { ps.data(), ps.size() };
    desc.InputLayout = { layout, 2 };

    // Rasterizer: no cull, no depth clip
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = FALSE;

    // Blend: no blending
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    // Depth: disabled
    desc.DepthStencilState.DepthEnable = FALSE;

    desc.SampleMask = UINT_MAX;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = config_.outputFormat;
    desc.DSVFormat = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;

    // Try loading from cache first
    pso_ = psoCache.loadGraphicsPSO(config_.debugName, desc);
    if (pso_) return true;

    // Cache miss -- compile
    HRESULT hr = device_->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(pso_.GetAddressOf()));
    if (FAILED(hr)) {
        LOG_ENGINE_ERROR("[D3D12] PostProcess PSO creation failed");
        return false;
    }

    psoCache.storePSO(config_.debugName, pso_.Get());
    return true;
}

bool D3D12PostProcessPass::createOutputTexture()
{
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = width_;
    texDesc.Height = height_;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = config_.outputFormat;
    texDesc.SampleDesc.Count = 1;
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = config_.outputFormat;
    clearValue.Color[0] = config_.clearColor[0];
    clearValue.Color[1] = config_.clearColor[1];
    clearValue.Color[2] = config_.clearColor[2];
    clearValue.Color[3] = config_.clearColor[3];

    HRESULT hr = device_->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clearValue,
        IID_PPV_ARGS(outputTexture_.GetAddressOf()));
    if (FAILED(hr)) {
        LOG_ENGINE_ERROR("[D3D12] PostProcess output texture creation failed");
        return false;
    }

    stateTracker_->track(outputTexture_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // RTV
    if (outputRTVIndex_ == UINT(-1))
        outputRTVIndex_ = rtvAllocator_->allocate();
    device_->CreateRenderTargetView(outputTexture_.Get(), nullptr,
                                     rtvAllocator_->getCPU(outputRTVIndex_));

    // SRV
    if (outputSRVIndex_ == UINT(-1))
        outputSRVIndex_ = srvAllocator_->allocate();
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = config_.outputFormat;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(outputTexture_.Get(), &srvDesc,
                                       srvAllocator_->getCPU(outputSRVIndex_));

    return true;
}

void D3D12PostProcessPass::destroyOutputTexture()
{
    if (outputTexture_) {
        stateTracker_->untrack(outputTexture_.Get());
        outputTexture_.Reset();
    }
    if (outputRTVIndex_ != UINT(-1) && rtvAllocator_) {
        rtvAllocator_->free(outputRTVIndex_);
        outputRTVIndex_ = UINT(-1);
    }
    if (outputSRVIndex_ != UINT(-1) && srvAllocator_) {
        srvAllocator_->free(outputSRVIndex_);
        outputSRVIndex_ = UINT(-1);
    }
}
