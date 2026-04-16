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
// SSAO Resource Creation (passes + input textures)
// ============================================================================

bool D3D12RenderAPI::createSSAOResources(int width, int height)
{
    if (!m_ssaoPass.isInitialized())
    {
        // First-time initialization: load shaders and create passes

        auto shaderBasePath = EnginePaths::resolveEngineAsset("../assets/shaders/compiled/d3d12/");
        auto ssaoVS = readShaderBinary(shaderBasePath + "ssao_vs.dxil");
        auto ssaoPS = readShaderBinary(shaderBasePath + "ssao_ps.dxil");
        auto ssaoBlurVS = readShaderBinary(shaderBasePath + "ssao_blur_vs.dxil");
        auto ssaoBlurPS = readShaderBinary(shaderBasePath + "ssao_blur_ps.dxil");

        if (ssaoVS.empty() || ssaoPS.empty() || ssaoBlurVS.empty() || ssaoBlurPS.empty())
        {
            LOG_ENGINE_ERROR("[D3D12] Failed to load SSAO shaders");
            return false;
        }

        // Shared config for all 3 SSAO passes
        D3D12PostProcessPassConfig cfg;
        cfg.outputFormat = DXGI_FORMAT_R8_UNORM;
        cfg.scaleFactor = 0.5f;
        cfg.useExternalRTV = false;
        cfg.clearColor[0] = 1.0f; cfg.clearColor[1] = 1.0f;
        cfg.clearColor[2] = 1.0f; cfg.clearColor[3] = 1.0f;

        cfg.bindings = {
            { D3D12PPBinding::CBV,       0, D3D12_SHADER_VISIBILITY_ALL   },  // b0: params
            { D3D12PPBinding::SRV_TABLE, 0, D3D12_SHADER_VISIBILITY_PIXEL },  // t0: depth / SSAO input
            { D3D12PPBinding::SRV_TABLE, 1, D3D12_SHADER_VISIBILITY_PIXEL },  // t1: noise / depth
        };

        // Static samplers: s0 and s1 both point clamp
        for (int i = 0; i < 2; i++)
        {
            D3D12_STATIC_SAMPLER_DESC samp = {};
            samp.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
            samp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            samp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            samp.ShaderRegister = i;
            samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            cfg.staticSamplers.push_back(samp);
        }

        // SSAO computation pass
        cfg.debugName = L"SSAO";
        if (!m_ssaoPass.init(device.Get(), m_rtvAllocator, m_srvAllocator,
                             m_stateTracker, m_psoCache, cfg,
                             width, height, ssaoVS, ssaoPS))
        {
            LOG_ENGINE_ERROR("[D3D12] Failed to create SSAO pass");
            return false;
        }

        // Horizontal blur pass
        cfg.debugName = L"SSAO Blur H";
        if (!m_ssaoBlurHPass.init(device.Get(), m_rtvAllocator, m_srvAllocator,
                                  m_stateTracker, m_psoCache, cfg,
                                  width, height, ssaoBlurVS, ssaoBlurPS))
        {
            LOG_ENGINE_ERROR("[D3D12] Failed to create SSAO blur H pass");
            return false;
        }

        // Vertical blur pass
        cfg.debugName = L"SSAO Blur V";
        if (!m_ssaoBlurVPass.init(device.Get(), m_rtvAllocator, m_srvAllocator,
                                  m_stateTracker, m_psoCache, cfg,
                                  width, height, ssaoBlurVS, ssaoBlurPS))
        {
            LOG_ENGINE_ERROR("[D3D12] Failed to create SSAO blur V pass");
            return false;
        }
    }
    else
    {
        // Resize existing passes
        m_ssaoPass.resize(width, height);
        m_ssaoBlurHPass.resize(width, height);
        m_ssaoBlurVPass.resize(width, height);
    }

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

        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

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

    LOG_ENGINE_INFO("[D3D12] SSAO resources created ({}x{} half-res)",
                    m_ssaoPass.getWidth(), m_ssaoPass.getHeight());
    return true;
}

// ============================================================================
// SSAO Render Pass
// ============================================================================

void D3D12RenderAPI::renderSSAOPass(ID3D12Resource* depthBuffer, UINT depthSRVIndex, int fullWidth, int fullHeight)
{
    if (!m_ssaoPass.isInitialized() || !depthBuffer) return;

    int halfW = static_cast<int>(m_ssaoPass.getWidth());
    int halfH = static_cast<int>(m_ssaoPass.getHeight());
    D3D12_GPU_VIRTUAL_ADDRESS cbAddr = 0;

    // --- Pass 1: SSAO computation ---

    transitionResource(depthBuffer, {}, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    flushBarriers();

    m_ssaoPass.begin(commandList.Get());

    float clearColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    commandList->ClearRenderTargetView(m_rtvAllocator.getCPU(m_ssaoPass.getOutputRTVIndex()),
                                       clearColor, 0, nullptr);

    {
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

        cbAddr = m_cbUploadBuffer[m_frameIndex].allocate(sizeof(ssaoCB), &ssaoCB);
        if (cbAddr == 0) goto ssao_cleanup;
    }

    commandList->SetGraphicsRootConstantBufferView(0, cbAddr);
    commandList->SetGraphicsRootDescriptorTable(1, m_srvAllocator.getGPU(depthSRVIndex));
    commandList->SetGraphicsRootDescriptorTable(2, m_srvAllocator.getGPU(m_ssaoNoiseSRVIndex));

    m_ssaoPass.draw(commandList.Get(), m_fxaaQuadVBV);

    // --- Pass 2: Horizontal blur ---

    transitionResource(m_ssaoPass.getOutputTexture(), {}, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    flushBarriers();

    m_ssaoBlurHPass.begin(commandList.Get());

    {
        D3D12SSAOBlurCBuffer blurCB = {};
        blurCB.texelSize = glm::vec2(1.0f / static_cast<float>(halfW), 1.0f / static_cast<float>(halfH));
        blurCB.blurDir = glm::vec2(1.0f, 0.0f); // horizontal
        blurCB.depthThreshold = 0.001f;

        cbAddr = m_cbUploadBuffer[m_frameIndex].allocate(sizeof(blurCB), &blurCB);
        if (cbAddr == 0) goto ssao_cleanup;

        commandList->SetGraphicsRootConstantBufferView(0, cbAddr);
        commandList->SetGraphicsRootDescriptorTable(1, m_srvAllocator.getGPU(m_ssaoPass.getOutputSRVIndex()));
        commandList->SetGraphicsRootDescriptorTable(2, m_srvAllocator.getGPU(depthSRVIndex));

        m_ssaoBlurHPass.draw(commandList.Get(), m_fxaaQuadVBV);
    }

    // --- Pass 3: Vertical blur ---

    transitionResource(m_ssaoBlurHPass.getOutputTexture(), {}, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    flushBarriers();

    m_ssaoBlurVPass.begin(commandList.Get());

    {
        D3D12SSAOBlurCBuffer blurCB = {};
        blurCB.texelSize = glm::vec2(1.0f / static_cast<float>(halfW), 1.0f / static_cast<float>(halfH));
        blurCB.blurDir = glm::vec2(0.0f, 1.0f); // vertical
        blurCB.depthThreshold = 0.001f;

        cbAddr = m_cbUploadBuffer[m_frameIndex].allocate(sizeof(blurCB), &blurCB);
        if (cbAddr == 0) goto ssao_cleanup;

        commandList->SetGraphicsRootConstantBufferView(0, cbAddr);
        commandList->SetGraphicsRootDescriptorTable(1, m_srvAllocator.getGPU(m_ssaoBlurHPass.getOutputSRVIndex()));
        commandList->SetGraphicsRootDescriptorTable(2, m_srvAllocator.getGPU(depthSRVIndex));

        m_ssaoBlurVPass.draw(commandList.Get(), m_fxaaQuadVBV);
    }

ssao_cleanup:
    // Transition all outputs to SRV (no-op if already in that state)
    transitionResource(m_ssaoPass.getOutputTexture(), {}, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transitionResource(m_ssaoBlurHPass.getOutputTexture(), {}, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transitionResource(m_ssaoBlurVPass.getOutputTexture(), {}, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transitionResource(depthBuffer, {}, D3D12_RESOURCE_STATE_DEPTH_WRITE);
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
