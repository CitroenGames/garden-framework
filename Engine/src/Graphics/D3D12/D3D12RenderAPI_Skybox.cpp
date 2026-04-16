#include "D3D12RenderAPI.hpp"
#include "Utils/Log.hpp"

// ============================================================================
// Skybox Pass Initialization
// ============================================================================

bool D3D12RenderAPI::createSkyboxPass(int width, int height)
{
    D3D12PostProcessPassConfig skyCfg;
    skyCfg.debugName = L"Sky";
    skyCfg.outputFormat = DXGI_FORMAT_R16G16B16A16_FLOAT; // HDR offscreen format
    skyCfg.useExternalRTV = true;

    skyCfg.bindings = {
        { D3D12PPBinding::CBV,       0, D3D12_SHADER_VISIBILITY_ALL   },  // b0: SkyboxCBuffer
        { D3D12PPBinding::SRV_TABLE, 0, D3D12_SHADER_VISIBILITY_PIXEL },  // t0: depth texture
    };

    // Point clamp sampler for depth
    D3D12_STATIC_SAMPLER_DESC depthSamp = {};
    depthSamp.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    depthSamp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    depthSamp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    depthSamp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    depthSamp.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    depthSamp.MaxLOD = D3D12_FLOAT32_MAX;
    depthSamp.ShaderRegister = 0;
    depthSamp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    skyCfg.staticSamplers.push_back(depthSamp);

    if (!m_skyPass.init(device.Get(), m_rtvAllocator, m_srvAllocator,
                        m_stateTracker, m_psoCache, skyCfg,
                        width, height, m_skyVS, m_skyPS)) {
        LOG_ENGINE_ERROR("Failed to create Sky post-process pass");
        return false;
    }

    return true;
}

// ============================================================================
// Skybox Rendering
// ============================================================================

void D3D12RenderAPI::renderSkybox()
{
    if (device_lost || !m_skyPass.isInitialized()) return;

    // Determine active depth buffer and SRV based on current render mode
    ID3D12Resource* depthBuffer = nullptr;
    UINT depthSRVIndex = UINT(-1);
    int currentWidth = 0;
    int currentHeight = 0;

    if (m_active_scene_target >= 0)
    {
        auto it = m_pie_viewports.find(m_active_scene_target);
        if (it != m_pie_viewports.end())
        {
            auto& pie = it->second;
            depthBuffer = pie.depthBuffer.Get();
            depthSRVIndex = pie.depthSRVIndex;
            currentWidth = pie.width;
            currentHeight = pie.height;
        }
    }
    else if (m_viewportTexture)
    {
        depthBuffer = m_viewportDepthBuffer.Get();
        depthSRVIndex = m_viewportDepthSRVIndex;
        currentWidth = viewport_width_rt;
        currentHeight = viewport_height_rt;
    }
    else
    {
        depthBuffer = m_depthStencilBuffer.Get();
        depthSRVIndex = m_depthSRVIndex;
        currentWidth = viewport_width;
        currentHeight = viewport_height;
    }

    if (!depthBuffer || depthSRVIndex == UINT(-1)) return;

    // Transition depth buffer for shader read
    transitionResource(depthBuffer, {}, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    flushBarriers();

    // Begin sky post-process pass (sets root sig, PSO, render target, viewport)
    m_skyPass.begin(commandList.Get(), m_currentRT.rtvHandle,
                    static_cast<uint32_t>(currentWidth),
                    static_cast<uint32_t>(currentHeight));

    // Upload constant buffer
    glm::mat4 viewNoTranslation = glm::mat4(glm::mat3(view_matrix));
    glm::mat4 vp = projection_matrix * viewNoTranslation;

    D3D12SkyboxCBuffer cb = {};
    cb.invViewProj = glm::inverse(vp);
    cb.sunDirection = -current_light_direction;
    cb._pad = 0.0f;

    auto cbAddr = m_cbUploadBuffer[m_frameIndex].allocate(sizeof(cb), &cb);
    if (cbAddr == 0) return;

    commandList->SetGraphicsRootConstantBufferView(0, cbAddr);
    commandList->SetGraphicsRootDescriptorTable(1, m_srvAllocator.getGPU(depthSRVIndex));

    // Draw fullscreen quad (reuse FXAA quad VB)
    m_skyPass.draw(commandList.Get(), m_fxaaQuadVBV);

    // Transition depth buffer back to depth-write
    transitionResource(depthBuffer, {}, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    flushBarriers();

    // Restore main render state
    commandList->OMSetRenderTargets(1, &m_currentRT.rtvHandle, FALSE, &m_currentRT.dsvHandle);
    commandList->RSSetViewports(1, &m_currentRT.viewport);
    commandList->RSSetScissorRects(1, &m_currentRT.scissor);
    commandList->SetGraphicsRootSignature(m_rootSignature.Get());
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    last_bound_pso = nullptr;
    global_cbuffer_dirty = true;
}
