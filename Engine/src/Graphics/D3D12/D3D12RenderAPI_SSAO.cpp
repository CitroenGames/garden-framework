#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "D3D12RenderAPI.hpp"
#include "Utils/Log.hpp"
#include "Utils/EnginePaths.hpp"
#include <cmath>
#include <random>
#include <algorithm>

// ============================================================================
// SSAO Kernel Generation
// ============================================================================

void D3D12RenderAPI::generateSSAOKernel()
{
    std::mt19937 rng(42); // Fixed seed for deterministic results
    std::uniform_real_distribution<float> dist01(0.0f, 1.0f);
    std::uniform_real_distribution<float> distNeg11(-1.0f, 1.0f);

    for (int i = 0; i < 16; i++)
    {
        // Random point in hemisphere (z >= 0)
        glm::vec3 sample(distNeg11(rng), distNeg11(rng), dist01(rng));
        sample = glm::normalize(sample);
        sample *= dist01(rng);

        // Scale: weight samples closer to the origin
        float scale = static_cast<float>(i) / 16.0f;
        scale = 0.1f + scale * scale * (1.0f - 0.1f); // lerp(0.1, 1.0, scale*scale)
        sample *= scale;

        ssaoKernel[i] = glm::vec4(sample, 0.0f);
    }
}

// ============================================================================
// SSAO Root Signature and Pipeline State Objects
// ============================================================================

bool D3D12RenderAPI::createSSAORootSignatureAndPSOs()
{
    // SSAO Root Signature:
    // [0] Root CBV b0 (SSAO params / blur params)
    // [1] Descriptor table: SRV t0 (depth texture or SSAO input for blur)
    // [2] Descriptor table: SRV t1 (noise texture or depth for blur)
    // Static samplers: s0 = point clamp, s1 = point wrap

    D3D12_ROOT_PARAMETER rootParams[3] = {};

    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[0].Descriptor.ShaderRegister = 0;
    rootParams[0].Descriptor.RegisterSpace = 0;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_DESCRIPTOR_RANGE srvRange0 = {};
    srvRange0.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange0.NumDescriptors = 1;
    srvRange0.BaseShaderRegister = 0;
    srvRange0.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[1].DescriptorTable.pDescriptorRanges = &srvRange0;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_DESCRIPTOR_RANGE srvRange1 = {};
    srvRange1.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange1.NumDescriptors = 1;
    srvRange1.BaseShaderRegister = 1;
    srvRange1.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    rootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[2].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[2].DescriptorTable.pDescriptorRanges = &srvRange1;
    rootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // Static samplers
    D3D12_STATIC_SAMPLER_DESC staticSamplers[2] = {};
    // s0: point clamp (for depth)
    staticSamplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    staticSamplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSamplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSamplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSamplers[0].ShaderRegister = 0;
    staticSamplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // s1: point wrap (for noise texture)
    staticSamplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    staticSamplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSamplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSamplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSamplers[1].ShaderRegister = 1;
    staticSamplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters = 3;
    rsDesc.pParameters = rootParams;
    rsDesc.NumStaticSamplers = 2;
    rsDesc.pStaticSamplers = staticSamplers;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> signature, error;
    HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
    if (FAILED(hr))
    {
        if (error)
            LOG_ENGINE_ERROR("[D3D12] SSAO root signature serialization failed: {}", (char*)error->GetBufferPointer());
        return false;
    }

    hr = device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                      IID_PPV_ARGS(m_ssaoRootSignature.GetAddressOf()));
    if (FAILED(hr)) return false;

    // Load SSAO shaders
    auto shaderBasePath = EnginePaths::resolveEngineAsset("../assets/shaders/compiled/d3d12/");
    m_ssaoVS = readShaderBinary(shaderBasePath + "ssao_vs.dxil");
    m_ssaoPS = readShaderBinary(shaderBasePath + "ssao_ps.dxil");
    m_ssaoBlurVS = readShaderBinary(shaderBasePath + "ssao_blur_vs.dxil");
    m_ssaoBlurPS = readShaderBinary(shaderBasePath + "ssao_blur_ps.dxil");

    if (m_ssaoVS.empty() || m_ssaoPS.empty() || m_ssaoBlurVS.empty() || m_ssaoBlurPS.empty())
    {
        LOG_ENGINE_ERROR("[D3D12] Failed to load SSAO shaders");
        return false;
    }

    // Vertex input layout (same as FXAA: pos2 + texcoord2)
    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    // SSAO PSO (renders to R8 target)
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = { inputLayout, 2 };
    psoDesc.pRootSignature = m_ssaoRootSignature.Get();
    psoDesc.VS = { m_ssaoVS.data(), m_ssaoVS.size() };
    psoDesc.PS = { m_ssaoPS.data(), m_ssaoPS.size() };
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8_UNORM;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.DepthStencilState.DepthEnable = FALSE;

    hr = device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(m_psoSSAO.GetAddressOf()));
    if (FAILED(hr))
    {
        LOG_ENGINE_ERROR("[D3D12] Failed to create SSAO PSO");
        return false;
    }

    // SSAO blur PSO (also renders to R8 target)
    psoDesc.VS = { m_ssaoBlurVS.data(), m_ssaoBlurVS.size() };
    psoDesc.PS = { m_ssaoBlurPS.data(), m_ssaoBlurPS.size() };

    hr = device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(m_psoSSAOBlur.GetAddressOf()));
    if (FAILED(hr))
    {
        LOG_ENGINE_ERROR("[D3D12] Failed to create SSAO blur PSO");
        return false;
    }

    LOG_ENGINE_INFO("[D3D12] SSAO root signature and PSOs created");
    return true;
}

// ============================================================================
// SSAO Resource Creation
// ============================================================================

bool D3D12RenderAPI::createSSAOResources(int width, int height)
{
    // Half resolution
    int halfW = std::max(1, width / 2);
    int halfH = std::max(1, height / 2);

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    // Create half-res R8 textures (raw, blur temp, blurred)
    auto createR8Texture = [&](ComPtr<ID3D12Resource>& tex, UINT& rtvIdx, UINT& srvIdx, const wchar_t* name) -> bool
    {
        if (tex)
            m_stateTracker.untrack(tex.Get());
        tex.Reset();

        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = halfW;
        desc.Height = halfH;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_CLEAR_VALUE clearValue = {};
        clearValue.Format = DXGI_FORMAT_R8_UNORM;
        clearValue.Color[0] = 1.0f;

        HRESULT hr = device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &clearValue,
            IID_PPV_ARGS(tex.GetAddressOf()));
        if (FAILED(hr)) return false;

        m_stateTracker.track(tex.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
        SetD3D12DebugName(tex.Get(), name);

        if (rtvIdx == UINT(-1)) rtvIdx = m_rtvAllocator.allocate();
        device->CreateRenderTargetView(tex.Get(), nullptr, m_rtvAllocator.getCPU(rtvIdx));

        if (srvIdx == UINT(-1)) srvIdx = m_srvAllocator.allocate();
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_R8_UNORM;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(tex.Get(), &srvDesc, m_srvAllocator.getCPU(srvIdx));

        return true;
    };

    if (!createR8Texture(m_ssaoRawTexture, m_ssaoRawRTVIndex, m_ssaoRawSRVIndex, L"SSAO Raw"))
        return false;
    if (!createR8Texture(m_ssaoBlurTempTexture, m_ssaoBlurTempRTVIndex, m_ssaoBlurTempSRVIndex, L"SSAO Blur Temp"))
        return false;
    if (!createR8Texture(m_ssaoBlurredTexture, m_ssaoBlurredRTVIndex, m_ssaoBlurredSRVIndex, L"SSAO Blurred"))
        return false;

    // Create depth buffer SRV (depth buffer is D24_UNORM_S8_UINT, we need SRV with R24_UNORM_X8_TYPELESS)
    if (m_depthSRVIndex == UINT(-1))
        m_depthSRVIndex = m_srvAllocator.allocate();

    D3D12_SHADER_RESOURCE_VIEW_DESC depthSrvDesc = {};
    depthSrvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    depthSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    depthSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    depthSrvDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(m_depthStencilBuffer.Get(), &depthSrvDesc,
                                      m_srvAllocator.getCPU(m_depthSRVIndex));

    // Create noise texture (4x4 RGBA32F with random rotation vectors)
    if (!m_ssaoNoiseTexture)
    {
        std::mt19937 rng(12345);
        std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

        float noiseData[16 * 4]; // 4x4 pixels * RGBA
        for (int i = 0; i < 16; i++)
        {
            float x = dist(rng);
            float y = dist(rng);
            float len = std::sqrt(x * x + y * y);
            if (len > 0.0001f) { x /= len; y /= len; }
            noiseData[i * 4 + 0] = x;
            noiseData[i * 4 + 1] = y;
            noiseData[i * 4 + 2] = 0.0f;
            noiseData[i * 4 + 3] = 0.0f;
        }

        D3D12_RESOURCE_DESC noiseDesc = {};
        noiseDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        noiseDesc.Width = 4;
        noiseDesc.Height = 4;
        noiseDesc.DepthOrArraySize = 1;
        noiseDesc.MipLevels = 1;
        noiseDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        noiseDesc.SampleDesc.Count = 1;

        HRESULT hr = device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &noiseDesc,
            D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(m_ssaoNoiseTexture.GetAddressOf()));
        if (FAILED(hr)) return false;

        SetD3D12DebugName(m_ssaoNoiseTexture.Get(), L"SSAO Noise");

        // Upload noise data via copy queue
        UINT64 rowPitch = AlignUp(4 * 16, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT); // 4 pixels * 16 bytes per pixel
        size_t uploadSize = AlignUp(rowPitch * 4, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);

        D3D12_HEAP_PROPERTIES uploadHeap = {};
        uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC uploadDesc = {};
        uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        uploadDesc.Width = uploadSize;
        uploadDesc.Height = 1;
        uploadDesc.DepthOrArraySize = 1;
        uploadDesc.MipLevels = 1;
        uploadDesc.SampleDesc.Count = 1;
        uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        ComPtr<ID3D12Resource> uploadBuffer;
        hr = device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
                                              D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                              IID_PPV_ARGS(uploadBuffer.GetAddressOf()));
        if (FAILED(hr)) return false;

        uint8_t* mapped = nullptr;
        uploadBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
        for (int y = 0; y < 4; y++)
            memcpy(mapped + y * rowPitch, &noiseData[y * 4 * 4], 4 * 16);
        uploadBuffer->Unmap(0, nullptr);

        auto* copyCmdList = m_copyQueue.getCommandList();

        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = m_ssaoNoiseTexture.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = uploadBuffer.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Offset = 0;
        src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        src.PlacedFootprint.Footprint.Width = 4;
        src.PlacedFootprint.Footprint.Height = 4;
        src.PlacedFootprint.Footprint.Depth = 1;
        src.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(rowPitch);

        copyCmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        m_copyQueue.retainStagingBuffer(std::move(uploadBuffer));
        m_copyQueue.addPendingTransition(m_ssaoNoiseTexture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        if (m_ssaoNoiseSRVIndex == UINT(-1))
            m_ssaoNoiseSRVIndex = m_srvAllocator.allocate();

        D3D12_SHADER_RESOURCE_VIEW_DESC noiseSrvDesc = {};
        noiseSrvDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        noiseSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        noiseSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        noiseSrvDesc.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(m_ssaoNoiseTexture.Get(), &noiseSrvDesc,
                                          m_srvAllocator.getCPU(m_ssaoNoiseSRVIndex));
    }

    // Fallback texture is created in createPostProcessingResources() (FXAA file)

    // Generate kernel samples
    generateSSAOKernel();

    LOG_ENGINE_INFO("[D3D12] SSAO resources created ({}x{})", halfW, halfH);
    return true;
}

// ============================================================================
// SSAO Render Pass
// ============================================================================

void D3D12RenderAPI::renderSSAOPass()
{
    if (!m_psoSSAO || !m_ssaoRawTexture || !m_depthStencilBuffer) return;

    int halfW = std::max(1, viewport_width / 2);
    int halfH = std::max(1, viewport_height / 2);

    // --- Pass 1: SSAO computation ---

    // Transition depth to SRV readable
    transitionResource(m_depthStencilBuffer.Get(), {}, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transitionResource(m_ssaoRawTexture.Get(), {}, D3D12_RESOURCE_STATE_RENDER_TARGET);
    flushBarriers();

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvAllocator.getCPU(m_ssaoRawRTVIndex);
    float clearColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
    commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

    D3D12_VIEWPORT vp = {};
    vp.Width = static_cast<float>(halfW);
    vp.Height = static_cast<float>(halfH);
    vp.MaxDepth = 1.0f;
    commandList->RSSetViewports(1, &vp);

    D3D12_RECT scissor = { 0, 0, static_cast<LONG>(halfW), static_cast<LONG>(halfH) };
    commandList->RSSetScissorRects(1, &scissor);

    commandList->SetGraphicsRootSignature(m_ssaoRootSignature.Get());
    commandList->SetPipelineState(m_psoSSAO.Get());
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

    // Upload SSAO constant buffer
    D3D12SSAOCBuffer ssaoCB = {};
    ssaoCB.projection = projection_matrix;
    ssaoCB.invProjection = glm::inverse(projection_matrix);
    for (int i = 0; i < 16; i++)
        ssaoCB.samples[i] = ssaoKernel[i];
    ssaoCB.screenSize = glm::vec2(static_cast<float>(halfW), static_cast<float>(halfH));
    ssaoCB.noiseScale = ssaoCB.screenSize / 4.0f;
    ssaoCB.radius = ssaoRadius;
    ssaoCB.bias = ssaoBias;
    ssaoCB.power = ssaoIntensity;

    auto cbAddr = m_cbUploadBuffer[m_frameIndex].allocate(sizeof(ssaoCB), &ssaoCB);
    if (cbAddr == 0) goto ssao_cleanup;

    commandList->SetGraphicsRootConstantBufferView(0, cbAddr);
    commandList->SetGraphicsRootDescriptorTable(1, m_srvAllocator.getGPU(m_depthSRVIndex));
    commandList->SetGraphicsRootDescriptorTable(2, m_srvAllocator.getGPU(m_ssaoNoiseSRVIndex));

    commandList->IASetVertexBuffers(0, 1, &m_fxaaQuadVBV);
    commandList->DrawInstanced(4, 1, 0, 0);

    // --- Pass 2: Horizontal blur ---

    transitionResource(m_ssaoRawTexture.Get(), {}, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transitionResource(m_ssaoBlurTempTexture.Get(), {}, D3D12_RESOURCE_STATE_RENDER_TARGET);
    flushBarriers();

    rtvHandle = m_rtvAllocator.getCPU(m_ssaoBlurTempRTVIndex);
    commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

    commandList->SetPipelineState(m_psoSSAOBlur.Get());

    {
        D3D12SSAOBlurCBuffer blurCB = {};
        blurCB.texelSize = glm::vec2(1.0f / static_cast<float>(halfW), 1.0f / static_cast<float>(halfH));
        blurCB.blurDir = glm::vec2(1.0f, 0.0f); // horizontal
        blurCB.depthThreshold = 0.001f;

        cbAddr = m_cbUploadBuffer[m_frameIndex].allocate(sizeof(blurCB), &blurCB);
        if (cbAddr == 0) goto ssao_cleanup;

        commandList->SetGraphicsRootConstantBufferView(0, cbAddr);
        commandList->SetGraphicsRootDescriptorTable(1, m_srvAllocator.getGPU(m_ssaoRawSRVIndex));
        commandList->SetGraphicsRootDescriptorTable(2, m_srvAllocator.getGPU(m_depthSRVIndex));

        commandList->DrawInstanced(4, 1, 0, 0);
    }

    // --- Pass 3: Vertical blur ---

    transitionResource(m_ssaoBlurTempTexture.Get(), {}, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transitionResource(m_ssaoBlurredTexture.Get(), {}, D3D12_RESOURCE_STATE_RENDER_TARGET);
    flushBarriers();

    rtvHandle = m_rtvAllocator.getCPU(m_ssaoBlurredRTVIndex);
    commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

    {
        D3D12SSAOBlurCBuffer blurCB = {};
        blurCB.texelSize = glm::vec2(1.0f / static_cast<float>(halfW), 1.0f / static_cast<float>(halfH));
        blurCB.blurDir = glm::vec2(0.0f, 1.0f); // vertical
        blurCB.depthThreshold = 0.001f;

        cbAddr = m_cbUploadBuffer[m_frameIndex].allocate(sizeof(blurCB), &blurCB);
        if (cbAddr == 0) goto ssao_cleanup;

        commandList->SetGraphicsRootConstantBufferView(0, cbAddr);
        commandList->SetGraphicsRootDescriptorTable(1, m_srvAllocator.getGPU(m_ssaoBlurTempSRVIndex));
        commandList->SetGraphicsRootDescriptorTable(2, m_srvAllocator.getGPU(m_depthSRVIndex));

        commandList->DrawInstanced(4, 1, 0, 0);
    }

ssao_cleanup:
    // Always restore state regardless of early exit
    transitionResource(m_ssaoBlurredTexture.Get(), {}, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transitionResource(m_depthStencilBuffer.Get(), {}, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    flushBarriers();

    // Restore the main root signature for the FXAA pass
    commandList->SetGraphicsRootSignature(m_rootSignature.Get());
}

// ============================================================================
// SSAO Settings
// ============================================================================

void D3D12RenderAPI::setSSAOEnabled(bool enabled) { ssaoEnabled = enabled; }
bool D3D12RenderAPI::isSSAOEnabled() const { return ssaoEnabled; }
void D3D12RenderAPI::setSSAORadius(float radius) { ssaoRadius = radius; }
void D3D12RenderAPI::setSSAOIntensity(float intensity) { ssaoIntensity = intensity; }
