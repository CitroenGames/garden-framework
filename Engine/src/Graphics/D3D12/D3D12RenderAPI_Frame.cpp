#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "D3D12RenderAPI.hpp"
#include "Components/camera.hpp"
#include "Utils/Log.hpp"
#include "imgui.h"
#include "imgui_impl_dx12.h"
#include <algorithm>
#include <cmath>

// ============================================================================
// Frame Management
// ============================================================================

void D3D12RenderAPI::beginFrame()
{
    if (device_lost) return;

    // Ensure command list is open (may already be open from shadow pass)
    // Note: deferred resource recreation happens inside ensureCommandListOpen()
    ensureCommandListOpen();

    // bindDummyRootParams() now restores the engine root signature internally,
    // so no matter what previous pass left behind, we end up with a known layout
    // before any root-CBV write.
    bindDummyRootParams();

    // Determine render target
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle;
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle;

    if (m_active_scene_target >= 0)
    {
        auto it = m_pie_viewports.find(m_active_scene_target);
        if (it != m_pie_viewports.end())
        {
            auto& pie = *it->second;
            transitionResource(pie.getHDR(), {}, D3D12_RESOURCE_STATE_RENDER_TARGET);
            flushBarriers();
            rtvHandle = m_rtvAllocator.getCPU(pie.getHDRRTV());
            dsvHandle = m_dsvAllocator.getCPU(pie.getDepthDSV());
            commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

            D3D12_VIEWPORT vp = {};
            vp.Width = static_cast<float>(pie.width());
            vp.Height = static_cast<float>(pie.height());
            vp.MaxDepth = 1.0f;
            commandList->RSSetViewports(1, &vp);

            D3D12_RECT scissor = { 0, 0, pie.width(), pie.height() };
            commandList->RSSetScissorRects(1, &scissor);

            m_currentRT.rtvHandle = rtvHandle;
            m_currentRT.dsvHandle = dsvHandle;
            m_currentRT.viewport = vp;
            m_currentRT.scissor = scissor;
            goto setup_done;
        }
    }

    if (m_editorViewport)
    {
        // Editor mode: render into the editor viewport's HDR + depth.
        auto& ev = *m_editorViewport;
        transitionResource(ev.getHDR(), {}, D3D12_RESOURCE_STATE_RENDER_TARGET);

        flushBarriers();
        rtvHandle = m_rtvAllocator.getCPU(ev.getHDRRTV());
        dsvHandle = m_dsvAllocator.getCPU(ev.getDepthDSV());
        commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

        D3D12_VIEWPORT vp = {};
        vp.Width = static_cast<float>(viewport_width_rt);
        vp.Height = static_cast<float>(viewport_height_rt);
        vp.MaxDepth = 1.0f;
        commandList->RSSetViewports(1, &vp);

        D3D12_RECT scissor = { 0, 0, viewport_width_rt, viewport_height_rt };
        commandList->RSSetScissorRects(1, &scissor);

        m_currentRT.rtvHandle = rtvHandle;
        m_currentRT.dsvHandle = dsvHandle;
        m_currentRT.viewport = vp;
        m_currentRT.scissor = scissor;
    }
    else
    {
        // Standalone: render into the client viewport's HDR, tone-map to back buffer.
        auto& cv = *m_clientViewport;
        transitionResource(cv.getHDR(), {}, D3D12_RESOURCE_STATE_RENDER_TARGET);

        flushBarriers();
        rtvHandle = m_rtvAllocator.getCPU(cv.getHDRRTV());
        dsvHandle = m_dsvAllocator.getCPU(cv.getDepthDSV());
        commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

        D3D12_VIEWPORT vp = {};
        vp.Width = static_cast<float>(viewport_width);
        vp.Height = static_cast<float>(viewport_height);
        vp.MaxDepth = 1.0f;
        commandList->RSSetViewports(1, &vp);

        D3D12_RECT scissor = { 0, 0, static_cast<LONG>(viewport_width), static_cast<LONG>(viewport_height) };
        commandList->RSSetScissorRects(1, &scissor);

        m_currentRT.rtvHandle = rtvHandle;
        m_currentRT.dsvHandle = dsvHandle;
        m_currentRT.viewport = vp;
        m_currentRT.scissor = scissor;
    }

setup_done:
    // Cache current render target state for parallel command list setup
    m_currentRT.valid = true;

    // Reset model matrix
    current_model_matrix = glm::mat4(1.0f);
    while (!model_matrix_stack.empty())
        model_matrix_stack.pop();

    // Reset state tracking
    last_bound_pso = nullptr;
    currentBoundTexture = INVALID_TEXTURE;
    in_depth_prepass = false;
    m_cachedLightCBAddr = 0;

    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Bind shadow map SRV
    if (m_shadowSRVIndex != UINT(-1))
        commandList->SetGraphicsRootDescriptorTable(3, m_srvAllocator.getGPU(m_shadowSRVIndex));

    // Flush global CBuffer if dirty
    if (global_cbuffer_dirty)
    {
        updateGlobalCBuffer();
        global_cbuffer_dirty = false;
    }
}

void D3D12RenderAPI::endFrame()
{
    if (device_lost) return;

    // Re-bind engine root signature (RmlUI may have overridden it)
    commandList->SetGraphicsRootSignature(m_rootSignature.Get());

    if (!m_editorViewport)
    {
        auto& cv = *m_clientViewport;

        bool wantSSAO = ssaoEnabled && m_ssaoBlurVPass.isInitialized();
        bool wantShadowMask = (shadowQuality > 0) && m_shadowMaskPass.isInitialized() && m_shadowMapArray;

        if (m_useRenderGraph)
        {
            D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvAllocator.getCPU(m_backBufferRTVs[m_backBufferIndex]);

            PostProcessGraphBuilder::Config cfg;
            cfg.width          = static_cast<uint32_t>(viewport_width);
            cfg.height         = static_cast<uint32_t>(viewport_height);
            cfg.wantSSAO       = wantSSAO;
            cfg.wantShadowMask = wantShadowMask;
            cfg.renderImGui    = true;
            cfg.renderRml      = true;

            if (m_useDeferred && m_gbufferPass.isInitialized()) {
                m_deferredGraphBuilder.setFrameInputs(rtvHandle,
                                                     cv.getHDR(),   cv.getHDRSRV(),   cv.getHDRRTV(),
                                                     cv.getDepth(), cv.getDepthSRV(), cv.getDepthDSV(),
                                                     m_backBuffers[m_backBufferIndex].Get(),
                                                     m_backBufferRTVs[m_backBufferIndex]);
                // Shadows are applied inside the deferred lighting pass, so the
                // screen-space ShadowMask is redundant (and would double-darken).
                // SSAO is left on — tonemap multiplies it into the lit HDR, same
                // as the forward path.
                PostProcessGraphBuilder::Config deferredCfg = cfg;
                deferredCfg.wantShadowMask = false;
                m_deferredGraphBuilder.build(m_frameGraph, m_rgBackend, deferredCfg);
            } else {
                m_ppGraphBuilder.setFrameInputs(rtvHandle,
                                                cv.getHDR(),   cv.getHDRSRV(),   cv.getHDRRTV(),
                                                cv.getDepth(), cv.getDepthSRV(), cv.getDepthDSV(),
                                                m_backBuffers[m_backBufferIndex].Get(),
                                                m_backBufferRTVs[m_backBufferIndex]);
                m_ppGraphBuilder.build(m_frameGraph, m_rgBackend, cfg);
            }

            m_skyboxRequested = false;
        }
        else
        {
            // Run SSAO pass before FXAA/tone-mapping (generates blurred SSAO texture)
            if (ssaoEnabled && m_ssaoPass.isInitialized())
                renderSSAOPass(cv.getDepth(), cv.getDepthSRV(), viewport_width, viewport_height);

            // Run shadow mask pass (generates shadow factor texture from depth + shadow maps)
            if (wantShadowMask)
                renderShadowMaskPass(cv.getDepth(), cv.getDepthSRV(), viewport_width, viewport_height);

            // Standalone: tone-map HDR offscreen to LDR back buffer (with optional FXAA)
            transitionResource(cv.getHDR(), {}, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            transitionResource(m_backBuffers[m_backBufferIndex].Get(), {}, D3D12_RESOURCE_STATE_RENDER_TARGET);
            flushBarriers();

            D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvAllocator.getCPU(m_backBufferRTVs[m_backBufferIndex]);
            renderFXAAPass(rtvHandle, cv.getHDRSRV(), viewport_width, viewport_height,
                           wantSSAO, wantShadowMask);

            commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

            // Render ImGui AFTER FXAA so UI text stays crisp (standalone mode only)
            ImDrawData* draw_data = ImGui::GetDrawData();
            if (draw_data)
            {
                transitionResource(m_backBuffers[m_backBufferIndex].Get(), {}, D3D12_RESOURCE_STATE_RENDER_TARGET);
                flushBarriers();
                ImGui_ImplDX12_RenderDrawData(draw_data, commandList.Get());
            }

            // Transition back buffer to present
            transitionResource(m_backBuffers[m_backBufferIndex].Get(), {}, D3D12_RESOURCE_STATE_PRESENT);
        }
    }

    // Close and execute command list
    flushBarriers();
    commandList->Close();
    m_commandListOpen = false;
    ID3D12CommandList* lists[] = { commandList.Get() };
    commandQueue->ExecuteCommandLists(1, lists);
}

void D3D12RenderAPI::present()
{
    if (device_lost) return;

    // Close and execute command list if still open
    // (editor flow: renderUI -> present, skipping endFrame)
    if (m_commandListOpen)
    {
        // Ensure back buffer is in PRESENT state
        transitionResource(m_backBuffers[m_backBufferIndex].Get(), {}, D3D12_RESOURCE_STATE_PRESENT);

        flushBarriers();
        commandList->Close();
        m_commandListOpen = false;
        ID3D12CommandList* lists[] = { commandList.Get() };
        commandQueue->ExecuteCommandLists(1, lists);
    }

    HRESULT hr = swapChain->Present(presentInterval, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
    {
        HRESULT reason = device->GetDeviceRemovedReason();
        LOG_ENGINE_ERROR("[D3D12] Device removed during Present (reason: 0x{:08X})", static_cast<unsigned>(reason));

        // Log DRED data if available
        ComPtr<ID3D12DeviceRemovedExtendedData> dred;
        if (SUCCEEDED(device.As(&dred)))
        {
            D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT breadcrumbs = {};
            if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&breadcrumbs)))
            {
                const D3D12_AUTO_BREADCRUMB_NODE* node = breadcrumbs.pHeadAutoBreadcrumbNode;
                while (node)
                {
                    if (node->pCommandListDebugNameW)
                    {
                        char name[256];
                        WideCharToMultiByte(CP_UTF8, 0, node->pCommandListDebugNameW, -1, name, 256, nullptr, nullptr);
                        LOG_ENGINE_ERROR("[D3D12] DRED breadcrumb - command list: {}, completed: {}/{}",
                                          name, *node->pLastBreadcrumbValue, node->BreadcrumbCount);
                    }
                    else
                    {
                        LOG_ENGINE_ERROR("[D3D12] DRED breadcrumb - (unnamed), completed: {}/{}",
                                          *node->pLastBreadcrumbValue, node->BreadcrumbCount);
                    }
                    node = node->pNext;
                }
            }

            D3D12_DRED_PAGE_FAULT_OUTPUT pageFault = {};
            if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&pageFault)))
            {
                LOG_ENGINE_ERROR("[D3D12] DRED page fault at VA: 0x{:016X}",
                                  pageFault.PageFaultVA);
            }
        }

        device_lost = true;
        return;
    }
    else if (FAILED(hr))
    {
        LOG_ENGINE_ERROR("[D3D12] Present failed (HRESULT: 0x{:08X})", static_cast<unsigned>(hr));
    }

    // Signal fence for this frame
    m_fenceValue++;
    m_frameContexts[m_frameIndex].fenceValue = m_fenceValue;
    commandQueue->Signal(m_fence.Get(), m_fenceValue);

    // Advance frame index
    m_frameIndex = (m_frameIndex + 1) % NUM_FRAMES_IN_FLIGHT;
}

void D3D12RenderAPI::clear(const glm::vec3& color)
{
    if (device_lost) return;

    float clearColor[4] = { color.r, color.g, color.b, 1.0f };

    if (m_active_scene_target >= 0)
    {
        auto it = m_pie_viewports.find(m_active_scene_target);
        if (it != m_pie_viewports.end())
        {
            auto& pie = *it->second;
            commandList->ClearRenderTargetView(m_rtvAllocator.getCPU(pie.getHDRRTV()), clearColor, 0, nullptr);
            commandList->ClearDepthStencilView(m_dsvAllocator.getCPU(pie.getDepthDSV()),
                                                D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
            return;
        }
    }

    if (m_editorViewport)
    {
        auto& ev = *m_editorViewport;
        commandList->ClearRenderTargetView(m_rtvAllocator.getCPU(ev.getHDRRTV()), clearColor, 0, nullptr);
        commandList->ClearDepthStencilView(m_dsvAllocator.getCPU(ev.getDepthDSV()),
                                            D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
    }
    else if (m_clientViewport)
    {
        // Standalone: clear the client viewport's HDR + depth
        auto& cv = *m_clientViewport;
        commandList->ClearRenderTargetView(m_rtvAllocator.getCPU(cv.getHDRRTV()), clearColor, 0, nullptr);
        commandList->ClearDepthStencilView(m_dsvAllocator.getCPU(cv.getDepthDSV()),
                                            D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
    }
}

// ============================================================================
// Camera / Matrix Operations
// ============================================================================

void D3D12RenderAPI::setCamera(const camera& cam)
{
    glm::vec3 pos = cam.getPosition();
    glm::vec3 target = cam.getTarget();
    glm::vec3 up = cam.getUpVector();
    view_matrix = glm::lookAt(pos, target, up);
    global_cbuffer_dirty = true;
}

void D3D12RenderAPI::pushMatrix() { model_matrix_stack.push(current_model_matrix); }

void D3D12RenderAPI::popMatrix()
{
    if (!model_matrix_stack.empty())
    {
        current_model_matrix = model_matrix_stack.top();
        model_matrix_stack.pop();
    }
}

void D3D12RenderAPI::translate(const glm::vec3& pos)
{
    current_model_matrix = glm::translate(current_model_matrix, pos);
}

void D3D12RenderAPI::rotate(const glm::mat4& rotation)
{
    current_model_matrix = current_model_matrix * rotation;
}

void D3D12RenderAPI::multiplyMatrix(const glm::mat4& matrix)
{
    current_model_matrix = current_model_matrix * matrix;
}

glm::mat4 D3D12RenderAPI::getProjectionMatrix() const { return projection_matrix; }
glm::mat4 D3D12RenderAPI::getViewMatrix() const { return view_matrix; }

// ============================================================================
// Constant Buffer Updates
// ============================================================================

void D3D12RenderAPI::updateGlobalCBuffer()
{
    D3D12GlobalCBuffer cb = {};
    cb.view = view_matrix;
    cb.projection = projection_matrix;
    for (int i = 0; i < NUM_CASCADES; i++)
        cb.lightSpaceMatrices[i] = lightSpaceMatrices[i];
    cb.cascadeSplits = glm::vec4(cascadeSplitDistances[0], cascadeSplitDistances[1],
                                  cascadeSplitDistances[2], cascadeSplitDistances[3]);
    cb.cascadeSplit4 = cascadeSplitDistances[NUM_CASCADES];
    cb.lightDir = current_light_direction;
    cb.cascadeCount = NUM_CASCADES;
    cb.lightAmbient = current_light_ambient;
    cb.lightDiffuse = current_light_diffuse;
    cb.debugCascades = debugCascades ? 1 : 0;
    cb.shadowMapTexelSize = glm::vec2(1.0f / static_cast<float>(currentShadowSize));

    auto addr = m_cbUploadBuffer[m_frameIndex].allocate(sizeof(cb), &cb);
    if (addr == 0) return;
    commandList->SetGraphicsRootConstantBufferView(0, addr);
}

void D3D12RenderAPI::updatePerObjectCBuffer(const glm::vec3& color, bool useTexture, float alphaCutoff)
{
    D3D12PerObjectCBuffer cb = {};
    cb.model = current_model_matrix;
    glm::mat3 normalMat3 = glm::mat3(current_model_matrix);
    float det = glm::determinant(normalMat3);
    if (std::abs(det) > 1e-6f)
        cb.normalMatrix = glm::mat4(glm::transpose(glm::inverse(normalMat3)));
    else
        cb.normalMatrix = glm::mat4(1.0f);
    cb.color = color;
    cb.useTexture = useTexture ? 1 : 0;
    cb.alphaCutoff = alphaCutoff;
    cb.metallic = 0.0f;
    cb.roughness = 0.5f;
    cb.emissive = glm::vec3(0.0f);
    cb.hasMetallicRoughnessMap = 0;
    cb.hasNormalMap = 0;
    cb.hasOcclusionMap = 0;
    cb.hasEmissiveMap = 0;

    auto addr = m_cbUploadBuffer[m_frameIndex].allocate(sizeof(cb), &cb);
    if (addr == 0) return;
    commandList->SetGraphicsRootConstantBufferView(1, addr);
}

void D3D12RenderAPI::updateShadowCBuffer(const glm::mat4& lightSpace, const glm::mat4& model)
{
    D3D12ShadowCBuffer cb = {};
    cb.lightSpaceMatrix = lightSpace;
    cb.model = model;

    auto addr = m_cbUploadBuffer[m_frameIndex].allocate(sizeof(cb), &cb);
    if (addr == 0) return;
    commandList->SetGraphicsRootConstantBufferView(0, addr);
}

// ============================================================================
// Render State / Lighting
// ============================================================================

void D3D12RenderAPI::setRenderState(const RenderState& state)
{
    current_state = state;
}

void D3D12RenderAPI::enableLighting(bool enable)
{
    lighting_enabled = enable;
}

void D3D12RenderAPI::setLighting(const glm::vec3& ambient, const glm::vec3& diffuse, const glm::vec3& direction)
{
    current_light_ambient = ambient;
    current_light_diffuse = diffuse;
    current_light_direction = glm::normalize(direction);
    global_cbuffer_dirty = true;
}

void D3D12RenderAPI::setPointAndSpotLights(const LightCBuffer& lights)
{
    current_lights = lights;
    m_cachedLightCBAddr = 0; // Force re-upload with new light data
}
